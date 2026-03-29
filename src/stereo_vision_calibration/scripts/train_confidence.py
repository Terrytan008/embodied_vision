#!/usr/bin/env python3
"""
Embodied Vision — 置信度估计网络训练
train_confidence.py

轻量级 CNN：输入 = 左目图 + 右目图 + 视差图
输出 = 每像素置信度图 (0.0~1.0)

Loss: Weighted BCE + 边缘保持正则
Dataset: KITTI / SceneFlow / FlyingThings3D
"""

import argparse
import os
import sys
import json
import time
from pathlib import Path
from typing import Tuple, Optional

import numpy as np
import cv2

import torch
import torch.nn as nn
import torch.nn.functional as F
from torch.utils.data import Dataset, DataLoader
from torch.utils.tensorboard import SummaryWriter
import torch.optim as optim
from torch.cuda.amp import autocast, GradScaler


# ============================================================================
# 网络架构：轻量级置信度估计 CNN
# ============================================================================

class ConvBlock(nn.Module):
    """卷积 + BN + ReLU"""
    def __init__(self, in_ch, out_ch, kernel_size=3, stride=1, padding=1):
        super().__init__()
        self.conv = nn.Conv2d(in_ch, out_ch, kernel_size, stride, padding, bias=False)
        self.bn = nn.BatchNorm2d(out_ch)
        self.relu = nn.ReLU(inplace=True)

    def forward(self, x):
        return self.relu(self.bn(self.conv(x)))


class ConfidenceNet(nn.Module):
    """
    轻量级置信度估计网络

    输入: [left, right, disparity] — 3通道
    输出: 1通道置信度图 (0.0~1.0)

    架构参考 U-Net 轻量化版本：
      - 编码器：4层下采样（提取语义）
      - 特征融合：3层上采样（恢复空间精度）
      - 输出：Sigmoid 激活
    """

    def __init__(self, base_channels=32):
        super().__init__()

        # ---- 编码器 ----
        self.enc1 = nn.Sequential(
            ConvBlock(3, base_channels, 3, 1, 1),      # H×W
            ConvBlock(base_channels, base_channels, 3, 1, 1),
        )
        self.enc2 = nn.Sequential(
            ConvBlock(base_channels, base_channels*2, 3, 2, 1),   # /2
            ConvBlock(base_channels*2, base_channels*2, 3, 1, 1),
        )
        self.enc3 = nn.Sequential(
            ConvBlock(base_channels*2, base_channels*4, 3, 2, 1),  # /4
            ConvBlock(base_channels*4, base_channels*4, 3, 1, 1),
        )
        self.enc4 = nn.Sequential(
            ConvBlock(base_channels*4, base_channels*8, 3, 2, 1),  # /8
            ConvBlock(base_channels*8, base_channels*8, 3, 1, 1),
        )

        # ---- Bottleneck ----
        self.bottleneck = nn.Sequential(
            ConvBlock(base_channels*8, base_channels*16, 3, 2, 1),  # /16
            ConvBlock(base_channels*16, base_channels*16, 3, 1, 1),
        )

        # ---- 解码器 ----
        self.up3 = nn.ConvTranspose2d(base_channels*16, base_channels*8, 2, 2)  # ×2
        self.dec3 = ConvBlock(base_channels*16, base_channels*8, 3, 1, 1)

        self.up2 = nn.ConvTranspose2d(base_channels*8, base_channels*4, 2, 2)
        self.dec2 = ConvBlock(base_channels*8, base_channels*4, 3, 1, 1)

        self.up1 = nn.ConvTranspose2d(base_channels*4, base_channels*2, 2, 2)
        self.dec1 = ConvBlock(base_channels*4, base_channels*2, 3, 1, 1)

        self.up0 = nn.ConvTranspose2d(base_channels*2, base_channels, 2, 2)
        self.dec0 = ConvBlock(base_channels*2, base_channels, 3, 1, 1)

        # ---- 输出层 ----
        self.confidence_head = nn.Sequential(
            ConvBlock(base_channels, base_channels//2, 3, 1, 1),
            nn.Conv2d(base_channels//2, 1, 1),
            nn.Sigmoid()
        )

    def forward(self, x):
        """
        Args:
            x: [B, 3, H, W] — left(1ch) + right(1ch) + disparity(1ch) 拼接
        Returns:
            conf: [B, 1, H, W] — 每像素置信度 (0.0~1.0)
        """
        # 编码器
        e1 = self.enc1(x)          # [B, 32, H, W]
        e2 = self.enc2(e1)         # [B, 64, H/2, W/2]
        e3 = self.enc3(e2)          # [B, 128, H/4, W/4]
        e4 = self.enc4(e3)          # [B, 256, H/8, W/8]

        # Bottleneck
        b = self.bottleneck(e4)     # [B, 512, H/16, W/16]

        # 解码器
        d3 = self.up3(b)            # [B, 256, H/8, W/8]
        d3 = self.dec3(torch.cat([d3, e4], dim=1))  # [B, 256, H/8, W/8]

        d2 = self.up2(d3)           # [B, 128, H/4, W/4]
        d2 = self.dec2(torch.cat([d2, e3], dim=1))

        d1 = self.up1(d2)           # [B, 64, H/2, W/2]
        d1 = self.dec1(torch.cat([d1, e2], dim=1))

        d0 = self.up0(d1)           # [B, 32, H, W]
        d0 = self.dec0(torch.cat([d0, e1], dim=1))

        # 输出
        conf = self.confidence_head(d0)  # [B, 1, H, W]
        return conf


# ============================================================================
# 数据集
# ============================================================================

class ConfidenceDataset(Dataset):
    """
    置信度训练数据集

    支持两种模式：
    1. StereoImgFolder: 从图像文件夹加载（left/right/depth/confidence）
    2. NumpyCache: 从预处理好的 numpy 缓存加载（更快）
    """

    def __init__(self,
                 data_root: str,
                 mode: str = "train",
                 img_h: int = 384,
                 img_w: int = 1280,
                 augment: bool = True):
        self.data_root = Path(data_root)
        self.mode = mode
        self.img_h = img_h
        self.img_w = img_w
        self.augment = augment and (mode == "train")

        # 支持数据集列表
        self.scene_dirs = sorted([
            d for d in self.data_root.iterdir()
            if d.is_dir()
        ])

        print(f"[Dataset] 找到 {len(self.scene_dirs)} 个场景目录")
        if len(self.scene_dirs) == 0:
            print(f"[警告] 数据目录为空: {data_root}")

    def __len__(self):
        return len(self.scene_dirs) * 100  # 每个场景重复100次/epoch

    def __getitem__(self, idx):
        scene_dir = self.scene_dirs[idx % len(self.scene_dirs)]

        # 尝试加载数据
        try:
            left_path = scene_dir / "left.png"
            right_path = scene_dir / "right.png"
            depth_path = scene_dir / "depth.npy"  # 真实深度（米）
            gt_conf_path = scene_dir / "confidence_gt.npy"  # 真实置信度

            left = cv2.imread(str(left_path), cv2.IMREAD_GRAYSCALE)
            right = cv2.imread(str(right_path), cv2.IMREAD_GRAYSCALE)
            depth = np.load(str(depth_path))
            gt_conf = np.load(str(gt_conf_path)) if gt_conf_path.exists() else None

        except Exception as e:
            # 降级：生成模拟数据（开发调试用）
            left = np.random.randint(0, 255, (self.img_h, self.img_w), dtype=np.uint8)
            right = np.random.randint(0, 255, (self.img_h, self.img_w), dtype=np.uint8)
            depth = np.ones((self.img_h, self.img_w), dtype=np.float32) * 2.0
            gt_conf = np.ones((self.img_h, self.img_w), dtype=np.float32) * 0.85

        # 数据增强
        if self.augment:
            left, right, depth, gt_conf = self._augment(
                left, right, depth, gt_conf)

        # 缩放到网络输入尺寸
        left = cv2.resize(left, (self.img_w, self.img_h))
        right = cv2.resize(right, (self.img_w, self.img_h))
        depth = cv2.resize(depth, (self.img_w, self.img_h),
                          interpolation=cv2.INTER_LINEAR)
        if gt_conf is not None:
            gt_conf = cv2.resize(gt_conf, (self.img_w, self.img_h),
                               interpolation=cv2.INTER_LINEAR)

        # 归一化
        left = left.astype(np.float32) / 255.0
        right = right.astype(np.float32) / 255.0

        # 视差归一化（假设最大视差256像素）
        disparity = np.clip(depth / 10.0 * 256.0, 0, 255).astype(np.float32) / 255.0

        # 拼接：left + right + disparity → 3通道
        input_img = np.stack([left, right, disparity], axis=0)  # [3, H, W]

        # GT置信度（如果不存在，从深度范围估算）
        if gt_conf is None:
            gt_conf = (depth > 0.1).astype(np.float32)  # 有深度的=1，否则=0

        return torch.from_numpy(input_img), torch.from_numpy(gt_conf).unsqueeze(0)

    def _augment(self, left, right, depth, gt_conf):
        """在线数据增强"""
        # 随机翻转
        if np.random.rand() > 0.5:
            left = cv2.flip(left, 1)
            right = cv2.flip(right, 1)
            depth = cv2.flip(depth, 1)
            if gt_conf is not None:
                gt_conf = cv2.flip(gt_conf, 1)

        # 随机亮度/对比度
        alpha = np.random.uniform(0.8, 1.2)   # 对比度
        beta = np.random.uniform(-0.1, 0.1)     # 亮度
        left = np.clip(left * alpha + beta * 255, 0, 255).astype(np.uint8)
        right = np.clip(right * alpha + beta * 255, 0, 255).astype(np.uint8)

        # 随机裁剪
        if np.random.rand() > 0.5:
            h, w = left.shape
            crop_h = np.random.randint(int(h*0.8), h)
            crop_w = np.random.randint(int(w*0.8), w)
            top = np.random.randint(0, h - crop_h)
            left = left[top:top+crop_h, :crop_w]
            right = right[top:top+crop_h, :crop_w]
            depth = depth[top:top+crop_h, :crop_w]
            if gt_conf is not None:
                gt_conf = gt_conf[top:top+crop_h, :crop_w]

        return left, right, depth, gt_conf


# ============================================================================
# Loss 函数
# ============================================================================

class ConfidenceLoss(nn.Module):
    """
    置信度 Loss = Weighted BCE + Edge Regularization + Depth Consistency

    设计原则：
    - 正样本（高置信）权重更高（避免模型总是预测低置信）
    - 边缘区域惩罚（避免在深度跳变处预测过高置信）
    - 深度不连续处应低置信
    """

    def __init__(self, pos_weight: float = 2.5):
        super().__init__()
        self.pos_weight = pos_weight

        # Sobel 梯度算子（用于边缘检测）
        self.register_buffer(
            "sobel_x",
            torch.tensor([[-1, 0, 1], [-2, 0, 2], [-1, 0, 1]],
                       dtype=torch.float32).view(1, 1, 3, 3) / 4.0
        )
        self.register_buffer(
            "sobel_y",
            torch.tensor([[-1, -2, -1], [0, 0, 0], [1, 2, 1]],
                       dtype=torch.float32).view(1, 1, 3, 3) / 4.0
        )

    def forward(self, pred_conf: torch.Tensor,
               gt_conf: torch.Tensor,
               depth: Optional[torch.Tensor] = None) -> dict:
        """
        Args:
            pred_conf: [B, 1, H, W] 预测置信度
            gt_conf: [B, 1, H, W] 真实置信度
            depth: [B, 1, H, W] 深度图（用于一致性正则，可选）
        Returns:
            loss_dict: {'total': scalar, 'bce': scalar, 'edge': scalar}
        """
        # 1. 加权 BCE
        # 忽略 GT=0（无效区域）的预测
        valid_mask = (gt_conf > 0).float()  # 有深度=有效
        pos_weight = torch.where(gt_conf > 0,
                                torch.tensor(self.pos_weight).to(gt_conf.device),
                                torch.tensor(1.0).to(gt_conf.device))

        bce = F.binary_cross_entropy(
            pred_conf, gt_conf, weight=pos_weight * valid_mask,
            reduction='sum'
        ) / (valid_mask.sum() + 1e-6)

        # 2. 边缘正则
        # 对 GT 低置信（=0 或接近0）的区域，预测高置信 → 惩罚
        # 深度跳变处（边缘）GT 应低置信
        if depth is not None:
            depth_grad_x = F.conv2d(depth, self.sobel_x, padding=1)
            depth_grad_y = F.conv2d(depth, self.sobel_y, padding=1)
            depth_grad_mag = torch.sqrt(depth_grad_x**2 + depth_grad_y**2 + 1e-6)

            # 深度不连续处 → 期望低置信
            edge_mask = (depth_grad_mag > 0.1).float()
            edge_penalty = (pred_conf * edge_mask * valid_mask).sum() / (edge_mask.sum() * valid_mask.sum() + 1e-6)
        else:
            edge_penalty = torch.tensor(0.0).to(pred_conf.device)

        # 3. 总损失
        total = bce + 0.05 * edge_penalty

        return {
            'total': total,
            'bce': bce,
            'edge_penalty': edge_penalty,
            'pred_mean': pred_conf.mean(),
        }


# ============================================================================
# 评估指标
# ============================================================================

def compute_metrics(pred_conf: torch.Tensor,
                   gt_conf: torch.Tensor,
                   threshold: float = 0.5) -> dict:
    """计算置信度估计性能指标"""
    pred_binary = (pred_conf > threshold).float()
    gt_binary = (gt_conf > threshold).float()

    # True Positive / False Positive / False Negative
    tp = (pred_binary * gt_binary).sum().item()
    fp = (pred_binary * (1 - gt_binary)).sum().item()
    fn = ((1 - pred_binary) * gt_binary).sum().item()
    tn = ((1 - pred_binary) * (1 - gt_binary)).sum().item()

    precision = tp / (tp + fp + 1e-6)
    recall = tp / (tp + fn + 1e-6)
    f1 = 2 * precision * recall / (precision + recall + 1e-6)
    accuracy = (tp + tn) / (tp + tn + fp + fn + 1e-6)

    # 预测均值（衡量是否过度保守）
    pred_mean = pred_conf.mean().item()

    return {
        'precision': precision,
        'recall': recall,
        'f1': f1,
        'accuracy': accuracy,
        'pred_mean': pred_mean,
        'gt_mean': gt_conf.mean().item(),
    }


# ============================================================================
# 训练循环
# ============================================================================

def train_one_epoch(model, dataloader, optimizer, loss_fn,
                    device, scaler, epoch, tb_writer) -> dict:
    model.train()
    total_loss = 0
    total_bce = 0
    total_edge = 0
    total_samples = 0

    for step, (input_img, gt_conf) in enumerate(dataloader):
        input_img = input_img.to(device)
        gt_conf = gt_conf.to(device)

        optimizer.zero_grad()

        # Mixed precision forward
        with autocast():
            pred_conf = model(input_img)  # [B, 1, H, W]
            loss_dict = loss_fn(pred_conf, gt_conf)
            loss = loss_dict['total']

        # Mixed precision backward
        scaler.scale(loss).backward()
        scaler.step(optimizer)
        scaler.update()

        batch_size = input_img.size(0)
        total_loss += loss.item() * batch_size
        total_bce += loss_dict['bce'].item() * batch_size
        total_edge += loss_dict['edge_penalty'].item() * batch_size
        total_samples += batch_size

        if step % 50 == 0:
            print(f"  [Epoch {epoch}] Step {step}/{len(dataloader)} | "
                  f"Loss: {loss.item():.4f} | "
                  f"BCE: {loss_dict['bce'].item():.4f} | "
                  f"Edge: {loss_dict['edge_penalty'].item():.4f}")

    # TensorBoard
    avg_loss = total_loss / total_samples
    if tb_writer:
        tb_writer.add_scalar('train/loss', avg_loss, epoch)
        tb_writer.add_scalar('train/bce', total_bce / total_samples, epoch)
        tb_writer.add_scalar('train/edge', total_edge / total_samples, epoch)

    return {'loss': avg_loss}


@torch.no_grad()
def evaluate(model, dataloader, loss_fn, device, epoch, tb_writer) -> dict:
    model.eval()
    total_loss = 0
    total_samples = 0
    all_metrics = []

    for input_img, gt_conf in dataloader:
        input_img = input_img.to(device)
        gt_conf = gt_conf.to(device)

        pred_conf = model(input_img)
        loss_dict = loss_fn(pred_conf, gt_conf)

        # 计算指标
        metrics = compute_metrics(pred_conf, gt_conf)
        all_metrics.append(metrics)

        batch_size = input_img.size(0)
        total_loss += loss_dict['total'].item() * batch_size
        total_samples += batch_size

    # 聚合指标
    avg_metrics = {
        'loss': total_loss / total_samples,
        'precision': np.mean([m['precision'] for m in all_metrics]),
        'recall': np.mean([m['recall'] for m in all_metrics]),
        'f1': np.mean([m['f1'] for m in all_metrics]),
        'accuracy': np.mean([m['accuracy'] for m in all_metrics]),
        'pred_mean': np.mean([m['pred_mean'] for m in all_metrics]),
    }

    if tb_writer:
        tb_writer.add_scalar('val/loss', avg_metrics['loss'], epoch)
        tb_writer.add_scalar('val/f1', avg_metrics['f1'], epoch)
        tb_writer.add_scalar('val/precision', avg_metrics['precision'], epoch)
        tb_writer.add_scalar('val/recall', avg_metrics['recall'], epoch)
        tb_writer.add_scalar('val/pred_mean', avg_metrics['pred_mean'], epoch)

    print(f"  [Val] Loss: {avg_metrics['loss']:.4f} | "
          f"F1: {avg_metrics['f1']:.4f} | "
          f"Precision: {avg_metrics['precision']:.4f} | "
          f"Recall: {avg_metrics['recall']:.4f} | "
          f"PredMean: {avg_metrics['pred_mean']:.4f}")

    return avg_metrics


# ============================================================================
# 主函数
# ============================================================================

def main():
    parser = argparse.ArgumentParser(description="置信度估计网络训练")
    parser.add_argument('--data_root', type=str,
                       default='./data/confidence_dataset',
                       help='数据集根目录')
    parser.add_argument('--output_dir', type=str,
                       default='./checkpoints/confidence_net',
                       help='模型输出目录')
    parser.add_argument('--epochs', type=int, default=50)
    parser.add_argument('--batch_size', type=int, default=8)
    parser.add_argument('--lr', type=float, default=1e-3)
    parser.add_argument('--img_h', type=int, default=384)
    parser.add_argument('--img_w', type=int, default=1280)
    parser.add_argument('--base_channels', type=int, default=32)
    parser.add_argument('--pos_weight', type=float, default=2.5,
                       help='正样本权重（BCE）')
    parser.add_argument('--resume', type=str, default=None,
                       help='恢复训练路径')
    parser.add_argument('--tensorboard_dir', type=str,
                       default='./runs/confidence_net')
    args = parser.parse_args()

    os.makedirs(args.output_dir, exist_ok=True)
    os.makedirs(args.tensorboard_dir, exist_ok=True)

    # ---- 设备 ----
    device = torch.device('cuda' if torch.cuda.is_available() else 'cpu')
    print(f"[设备] {device}")

    # ---- 数据 ----
    train_dataset = ConfidenceDataset(
        args.data_root, mode='train',
        img_h=args.img_h, img_w=args.img_w, augment=True)
    val_dataset = ConfidenceDataset(
        args.data_root, mode='val',
        img_h=args.img_h, img_w=args.img_w, augment=False)

    train_loader = DataLoader(train_dataset, batch_size=args.batch_size,
                             shuffle=True, num_workers=4, pin_memory=True)
    val_loader = DataLoader(val_dataset, batch_size=args.batch_size,
                           shuffle=False, num_workers=4, pin_memory=True)

    print(f"[数据] Train: {len(train_dataset)}  Val: {len(val_dataset)}")

    # ---- 模型 ----
    model = ConfidenceNet(base_channels=args.base_channels).to(device)
    total_params = sum(p.numel() for p in model.parameters())
    trainable_params = sum(p.numel() for p in model.parameters() if p.requires_grad)
    print(f"[模型] ConfidenceNet (base_ch={args.base_channels})")
    print(f"       参数量: {total_params/1e6:.2f}M ({trainable_params/1e6:.2f}M 可训练)")

    # ---- 优化器 / Loss / Scaler ----
    optimizer = optim.AdamW(model.parameters(), lr=args.lr,
                           weight_decay=1e-4)
    scheduler = optim.lr_scheduler.CosineAnnealingLR(
        optimizer, T_max=args.epochs, eta_min=1e-6)
    loss_fn = ConfidenceLoss(pos_weight=args.pos_weight).to(device)
    scaler = GradScaler()

    # ---- TensorBoard ----
    tb_writer = SummaryWriter(args.tensorboard_dir)

    # ---- 恢复训练 ----
    start_epoch = 0
    best_f1 = 0.0
    if args.resume:
        checkpoint = torch.load(args.resume, map_location=device)
        model.load_state_dict(checkpoint['model_state_dict'])
        optimizer.load_state_dict(checkpoint['optimizer_state_dict'])
        scheduler.load_state_dict(checkpoint['scheduler_state_dict'])
        start_epoch = checkpoint['epoch'] + 1
        best_f1 = checkpoint.get('best_f1', 0.0)
        print(f"[恢复] Epoch {start_epoch}, Best F1: {best_f1:.4f}")

    # ---- 训练循环 ----
    print(f"\n{'='*60}")
    print(f"开始训练: {args.epochs} epochs")
    print(f"{'='*60}")

    for epoch in range(start_epoch, args.epochs):
        epoch_start = time.time()

        # Train
        train_metrics = train_one_epoch(
            model, train_loader, optimizer, loss_fn,
            device, scaler, epoch, tb_writer)

        # Validate
        val_metrics = evaluate(
            model, val_loader, loss_fn, device, epoch, tb_writer)

        scheduler.step()

        # 保存检查点
        ckpt_path = os.path.join(args.output_dir, f'epoch_{epoch:03d}.pt')
        torch.save({
            'epoch': epoch,
            'model_state_dict': model.state_dict(),
            'optimizer_state_dict': optimizer.state_dict(),
            'scheduler_state_dict': scheduler.state_dict(),
            'val_f1': val_metrics['f1'],
            'val_loss': val_metrics['loss'],
            'best_f1': best_f1,
            'args': vars(args),
        }, ckpt_path)

        # 保存最优模型
        if val_metrics['f1'] > best_f1:
            best_f1 = val_metrics['f1']
            best_path = os.path.join(args.output_dir, 'best_f1.pt')
            torch.save({
                'epoch': epoch,
                'model_state_dict': model.state_dict(),
                'val_f1': val_metrics['f1'],
                'args': vars(args),
            }, best_path)
            print(f"  ★ 新最优 F1: {best_f1:.4f} → {best_path}")

        # 每10个epoch打印一次
        if epoch % 10 == 0 or epoch == args.epochs - 1:
            elapsed = time.time() - epoch_start
            print(f"\n[Epoch {epoch}/{args.epochs}] "
                  f"Train Loss: {train_metrics['loss']:.4f} | "
                  f"Val Loss: {val_metrics['loss']:.4f} | "
                  f"F1: {val_metrics['f1']:.4f} | "
                  f"Best F1: {best_f1:.4f} | "
                  f"Time: {elapsed:.1f}s\n")

    # ---- 最终导出 ----
    export_path = os.path.join(args.output_dir, 'confidence_net_final.pt')
    torch.save({
        'model_state_dict': model.state_dict(),
        'args': vars(args),
    }, export_path)
    print(f"[完成] 最终模型: {export_path}")
    print(f"[完成] 最优 F1: {best_f1:.4f}")

    tb_writer.close()


if __name__ == '__main__':
    main()
