#!/usr/bin/env python3
"""
Embodied Vision — 置信度网络导出工具
export_onnx.py

将 PyTorch 置信度模型导出为 ONNX 格式，
供 C++ 推理引擎使用（stmCaffe / TensorRT / ONNXRuntime）
"""

import argparse
import sys
import torch
import numpy as np

# 添加项目路径
sys.path.insert(0, str(__file__).rsplit('/', 2)[0])

from train_confidence import ConfidenceNet


def export_onnx(model_ckpt: str,
                output_path: str,
                img_h: int = 384,
                img_w: int = 1280,
                opset_version: int = 13,
                simplify: bool = True):
    """
    导出为 ONNX

    Args:
        model_ckpt: .pt 检查点路径
        output_path: 输出 .onnx 路径
        img_h/img_w: 输入分辨率
        opset_version: ONNX opset 版本（13=支持Tensorshape算子）
        simplify: 是否用 onnxsim 简化
    """
    print(f"[导出] 加载模型: {model_ckpt}")

    # 加载检查点
    ckpt = torch.load(model_ckpt, map_location='cpu')

    # 获取参数字典
    if 'model_state_dict' in ckpt:
        state_dict = ckpt['model_state_dict']
        args = ckpt.get('args', {})
    else:
        state_dict = ckpt
        args = {}

    base_channels = args.get('base_channels', 32)

    # 构建模型
    model = ConfidenceNet(base_channels=base_channels)
    model.load_state_dict(state_dict)
    model.eval()

    print(f"[模型] base_channels={base_channels}")
    print(f"[输入] H={img_h} W={img_w}")

    # 创建示例输入
    dummy_input = torch.randn(1, 3, img_h, img_w)

    # ONNX 导出
    print(f"[导出] 生成 ONNX: {output_path}")
    torch.onnx.export(
        model,
        dummy_input,
        output_path,
        opset_version=opset_version,
        input_names=['input'],       # left+right+disparity 3通道
        output_names=['confidence'],  # 置信度输出
        dynamic_axes={
            'input': {2: 'height', 3: 'width'},
            'confidence': {2: 'height', 3: 'width'},
        },
        verbose=False,
        do_constant_folding=True,
    )
    print(f"[OK] ONNX 导出成功: {output_path}")

    # 验证 ONNX 推理
    try:
        import onnx
        onnx_model = onnx.load(output_path)
        onnx.checker.check_model(onnx_model)
        print("[验证] ONNX 模型检查通过")

        # 推理验证
        import onnxruntime as ort
        sess = ort.InferenceSession(output_path)
        input_onnx = {sess.get_inputs()[0].name: dummy_input.numpy()}
        output_onnx = sess.run(None, input_onnx)
        print(f"[验证] ONNX 推理输出形状: {output_onnx[0].shape}")
        print(f"[验证] ONNX 输出范围: [{output_onnx[0].min():.4f}, {output_onnx[0].max():.4f}]")

    except ImportError:
        print("[警告] onnx 或 onnxruntime 未安装，跳过验证")

    # Simplify（可选）
    if simplify:
        try:
            import onnxsim
            print("[简化] 运行 onnxsim...")
            onnx_model = onnx.load(output_path)
            simplified, check = onnxsim.simplify(onnx_model)
            if check:
                onnx.save(simplified, output_path)
                print(f"[简化] 简化成功: {output_path}")
            else:
                print("[简化] 简化失败，保持原模型")
        except ImportError:
            print("[提示] onnxsim 未安装，跳过简化")
            print("       安装: pip install onnxsim")

    # 打印模型大小
    import os
    size_mb = os.path.getsize(output_path) / 1e6
    print(f"[大小] {size_mb:.2f} MB")
    return output_path


def export_tensorrt(model_ckpt: str,
                     output_path: str,
                     img_h: int = 384,
                     img_w: int = 1280):
    """
    导出为 TensorRT Engine（需要 tensorrt）
    """
    try:
        import tensorrt as trt
        import pycuda.driver as cuda
    except ImportError:
        print("[错误] TensorRT 未安装，无法导出 TensorRT Engine")
        print("       请先安装: pip install tensorrt")
        return None

    print(f"[TensorRT] 从 ONNX 生成 Engine: {model_ckpt}")

    # 先导出 ONNX
    onnx_path = output_path.replace('.trt', '.onnx')
    if not os.path.exists(onnx_path):
        export_onnx(model_ckpt, onnx_path, img_h, img_w, simplify=False)

    # TensorRT Builder
    logger = trt.Logger(trt.Logger.WARNING)
    builder = trt.Builder(logger)
    network = builder.create_network(
        1 << int(trt.NetworkDefinitionCreationFlag.EXPLICIT_BATCH))
    parser = trt.OnnxParser(network, logger)

    with open(onnx_path, 'rb') as f:
        parser.parse(f.read())

    config = builder.create_builder_config()
    config.set_memory_pool_limit(trt.MemoryPoolType.WORKSPACE, 1 << 30)  # 1GB

    # FP16 加速
    if builder.platform_has_fast_fp16:
        config.set_flag(trt.BuilderFlag.FP16)
        print("[TensorRT] 启用 FP16 加速")

    engine_bytes = builder.build_serialized_network(network, config)
    with open(output_path, 'wb') as f:
        f.write(engine_bytes)

    print(f"[TensorRT] Engine 生成成功: {output_path}")
    return output_path


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description='导出置信度网络')
    parser.add_argument('--checkpoint', '-c', required=True,
                      help='PyTorch .pt 检查点路径')
    parser.add_argument('--output', '-o', default='confidence_net.onnx',
                      help='输出路径 (.onnx 或 .trt)')
    parser.add_argument('--img_h', type=int, default=384)
    parser.add_argument('--img_w', type=int, default=1280)
    parser.add_argument('--opset', type=int, default=13,
                      help='ONNX opset 版本')
    parser.add_argument('--no_simplify', action='store_true',
                      help='禁用 onnxsim 简化')
    parser.add_argument('--tensorrt', action='store_true',
                      help='同时生成 TensorRT Engine')

    args = parser.parse_args()

    # 推断格式
    is_trt = args.output.endswith('.trt')

    if is_trt or args.tensorrt:
        export_tensorrt(args.checkpoint, args.output,
                      args.img_h, args.img_w)
    else:
        export_onnx(args.checkpoint, args.output,
                   args.img_h, args.img_w,
                   opset_version=args.opset,
                   simplify=not args.no_simplify)
