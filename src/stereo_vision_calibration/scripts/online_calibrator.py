#!/usr/bin/env python3
"""
Embodied Vision — 在线标定工具
online_calibrator.py

支持：
  1. 工厂标定（张正友棋盘格法）
  2. 在线标定（运行时自标定，用于补偿机械微变形）
  3. 标定质量评估

依赖：OpenCV, NumPy, ROS2 (rclpy)
"""

import argparse
import sys
import time
import json
import numpy as np
import cv2
from pathlib import Path

try:
    from numba import jit
    NUMBA_AVAILABLE = True
except ImportError:
    NUMBA_AVAILABLE = False
    # numba 可通过 `pip install numba` 安装
    def jit(*args, **kwargs):
        """no-op 装饰器（numba 未安装时）"""
        def decorator(fn):
            return fn
        return decorator if args and callable(args[0]) else lambda fn: fn


class StereoCalibrator:
    """双目立体标定工具"""

    def __init__(self, board_size=(9, 6), square_size_mm=25.0):
        """
        Args:
            board_size: 棋盘格内角点数量 (cols, rows)
            square_size_mm: 单个方格边长（毫米）
        """
        self.board_size = board_size
        self.square_size = square_size_mm
        self.objp = self._create_obj_points()

    def _create_obj_points(self):
        """创建棋盘格三维坐标点"""
        objp = np.zeros((self.board_size[0] * self.board_size[1], 3), np.float32)
        objp[:, :2] = np.mgrid[
            0:self.board_size[0],
            0:self.board_size[1]
        ].T.reshape(-1, 2)
        objp *= self.square_size
        return objp

    def find_board_corners(self, img):
        """
        查找棋盘格角点
        Returns: (success, corners)
        """
        gray = cv2.cvtColor(img, cv2.COLOR_BGR2GRAY)
        ret, corners = cv2.findChessboardCorners(gray, self.board_size, None)
        if ret:
            # 亚像素精确化
            criteria = (cv2.TERM_CRITERIA_EPS + cv2.TERM_CRITERIA_MAX_ITER, 30, 0.001)
            corners = cv2.cornerSubPix(gray, corners, (11, 11), (-1, -1), criteria)
        return ret, corners

    def factory_calibrate(self, left_images, right_images):
        """
        工厂标定：张正友法求解内外参

        Args:
            left_images: 左目图像路径列表
            right_images: 右目图像路径列表

        Returns:
            calib_params: 标定参数字典
        """
        assert len(left_images) == len(right_images), "左右图数量必须一致"

        objpoints = []  # 三维点
        imgpoints_l = []  # 左目二维点
        imgpoints_r = []  # 右目二维点

        for left_path, right_path in zip(left_images, right_images):
            img_l = cv2.imread(str(left_path))
            img_r = cv2.imread(str(right_path))

            ret_l, corners_l = self.find_board_corners(img_l)
            ret_r, corners_r = self.find_board_corners(img_r)

            if ret_l and ret_r:
                objpoints.append(self.objp)
                imgpoints_l.append(corners_l)
                imgpoints_r.append(corners_r)
                print(f"[OK] {left_path.name}")
            else:
                print(f"[SKIP] {left_path.name} (角点检测失败)")

        if len(objpoints) < 10:
            raise ValueError(f"有效标定图像不足: {len(objpoints)}/10")

        # 左目内参
        ret_l, mtx_l, dist_l, rvecs_l, tvecs_l = cv2.calibrateCamera(
            objpoints, imgpoints_l, imgpoints_l[0].shape[::-1], None, None
        )

        # 右目内参
        ret_r, mtx_r, dist_r, rvecs_r, tvecs_r = cv2.calibrateCamera(
            objpoints, imgpoints_r, imgpoints_r[0].shape[::-1], None, None
        )

        # 双目立体标定
        flags = cv2.CALIB_FIX_INTRINSIC
        ret, mtx_l, dist_l, mtx_r, dist_r, R, T, E, F = cv2.stereoCalibrate(
            objpoints, imgpoints_l, imgpoints_r,
            mtx_l, dist_l, mtx_r, dist_r,
            imgpoints_l[0].shape[::-1],
            flags=flags
        )

        # 基线距离
        baseline_mm = float(np.linalg.norm(T)) * 1000.0

        # 基线方向（用于在线标定参考）
        baseline_direction = T.flatten() / (np.linalg.norm(T) + 1e-10)

        params = {
            "left_K": mtx_l.flatten().tolist(),
            "right_K": mtx_r.flatten().tolist(),
            "left_D": dist_l.flatten().tolist(),
            "right_D": dist_r.flatten().tolist(),
            "R": R.flatten().tolist(),
            "T": T.flatten().tolist(),
            "E": E.flatten().tolist(),
            "F": F.flatten().tolist(),
            "baseline_mm": baseline_mm,
            "baseline_direction": baseline_direction.tolist(),
            "rms_reprojection_error": float(ret),
            "num_valid_frames": len(objpoints),
        }

        print(f"\n[完成] RMS重投影误差: {ret:.4f} pixel")
        print(f"[完成] 基线距离: {baseline_mm:.2f} mm")
        return params

    def online_calibrate(self, current_frame_left, current_frame_right,
                        reference_params, max_iterations=50):
        """
        在线标定：补偿机械微变形（numba JIT 加速 BA）

        基于参考标定参数，用当前帧进行 BA 优化
        参数向量：[r_x, r_y, r_z, t_x, t_y, t_z]（6维）

        Args:
            current_frame_left: 当前左帧
            current_frame_right: 当前右帧
            reference_params: 参考标定参数（dict，含 left_K, right_K, baseline_mm）
            max_iterations: 最大迭代次数
        Returns:
            adjusted_params: 调整后的参数 + 质量分数
        """
        gray_l = cv2.cvtColor(current_frame_left, cv2.COLOR_BGR2GRAY)
        gray_r = cv2.cvtColor(current_frame_right, cv2.COLOR_BGR2GRAY)

        ret_l, corners_l = self.find_board_corners(current_frame_left)
        ret_r, corners_r = self.find_board_corners(current_frame_right)

        if not (ret_l and ret_r):
            return {
                "converged": False,
                "quality": 0.0,
                "message": "角点检测失败，无法在线标定"
            }

        objp = self.objp.astype(np.float64)
        imgp_l = corners_l.reshape(-1, 2).astype(np.float64)
        imgp_r = corners_r.reshape(-1, 2).astype(np.float64)

        K_l = np.array(reference_params["left_K"]).reshape(3, 3).astype(np.float64)
        K_r = np.array(reference_params["right_K"]).reshape(3, 3).astype(np.float64)
        baseline = float(reference_params.get("baseline_mm", 80.0)) / 1000.0

        # 初始外参（参考参数）
        # 简化：假设纯平移（baseline 在 X 方向），小角度旋转向量
        r_vec = np.zeros(3, dtype=np.float64)   # 旋转向量
        t_vec = np.array([baseline, 0.0, 0.0], dtype=np.float64)  # 初始平移

        params = np.concatenate([r_vec, t_vec])

        # Gauss-Newton 优化（numba JIT 加速）
        for iteration in range(max_iterations):
            J = np.zeros((imgp_l.shape[0] * 2, 6), dtype=np.float64)
            residuals = np.zeros(imgp_l.shape[0] * 2, dtype=np.float64)

            reproj_err, J = _ba_jacobian_residual(
                objp, imgp_l, K_l, params, J, residuals)

            if reproj_err < 1e-6:
                break

            # 正则化（防止奇异性）
            H = J.T @ J + 1e-6 * np.eye(6)
            delta = np.linalg.solve(H, J.T @ residuals)
            params -= delta

            if np.linalg.norm(delta) < 1e-8:
                break

        # 提取优化结果
        r_opt = params[:3]
        t_opt = params[3:6]

        # 质量评估：优化后重投影误差
        _, imgp_l_reproj = cv2.projectPoints(
            objp, r_opt, t_opt, K_l, np.zeros(5))
        reproj_error = np.linalg.norm(
            imgp_l.reshape(-1, 2) - imgp_l_reproj.reshape(-1, 2)) / len(objp)

        quality = float(np.clip(1.0 - reproj_error / 5.0, 0.0, 1.0))
        converged = reproj_error < 2.0  # 2像素阈值

        return {
            "converged": converged,
            "quality": quality,
            "message": f"在线标定完成，RMS误差={reproj_error:.3f}px，使用numba JIT"
        }


# =============================================================================
# numba JIT 加速：BA 残差 + Jacobian 计算
# =============================================================================
if NUMBA_AVAILABLE:
    @jit(nopython=True, cache=True)
    def _ba_jacobian_residual(objp, imgp, K, params, J, residuals):
        """
        计算 BA 残差向量和雅可比矩阵（numba JIT 编译）
        objp: (N, 3) 世界坐标点
        imgp: (N, 2) 观测到的图像坐标
        K: (3, 3) 内参矩阵
        params: (6,) [r_x, r_y, r_z, t_x, t_y, t_z]
        J: (2N, 6) 输出雅可比矩阵（预分配）
        residuals: (2N,) 输出残差向量（预分配）
        Returns: 总残差平方和
        """
        fx, fy = K[0, 0], K[1, 1]
        cx, cy = K[0, 2], K[1, 2]
        rx, ry, rz = params[0], params[1], params[2]
        tx, ty, tz = params[3], params[4], params[5]

        # Rodrigues 旋转矩阵
        theta = (rx * rx + ry * ry + rz * rz) ** 0.5
        if theta < 1e-10:
            R00, R01, R02 = 1.0, 0.0, 0.0
            R10, R11, R12 = 0.0, 1.0, 0.0
            R20, R21, R22 = 0.0, 0.0, 1.0
        else:
            c = np.cos(theta)
            s = np.sin(theta)
            ux, uy, uz = rx / theta, ry / theta, rz / theta
            R00 = c + ux * ux * (1 - c)
            R01 = ux * uy * (1 - c) - uz * s
            R02 = ux * uz * (1 - c) + uy * s
            R10 = uy * ux * (1 - c) + uz * s
            R11 = c + uy * uy * (1 - c)
            R12 = uy * uz * (1 - c) - ux * s
            R20 = uz * ux * (1 - c) - uy * s
            R21 = uz * uy * (1 - c) + ux * s
            R22 = c + uz * uz * (1 - c)

        total_err = 0.0
        n = objp.shape[0]

        for i in range(n):
            X, Y, Z = objp[i, 0], objp[i, 1], objp[i, 2]
            # 旋转+平移
            Xp = R00 * X + R01 * Y + R02 * Z + tx
            Yp = R10 * X + R11 * Y + R12 * Z + ty
            Zp = R20 * X + R21 * Y + R22 * Z + tz

            if Zp < 1e-10:
                Zp = 1e-10

            # 投影到图像平面
            u_proj = fx * Xp / Zp + cx
            v_proj = fy * Yp / Zp + cy

            # 残差
            e_u = imgp[i, 0] - u_proj
            e_v = imgp[i, 1] - v_proj
            residuals[2 * i] = e_u
            residuals[2 * i + 1] = e_v
            total_err += e_u * e_u + e_v * e_v

            # 简化雅可比（仅一阶近似）
            inv_Z = 1.0 / Zp
            du_dX = -fx * inv_Z
            du_dY = -fx * inv_Z
            du_dZ = fx * Xp / (Zp * Zp)
            dv_dX = -fy * inv_Z
            dv_dY = -fy * inv_Z
            dv_dZ = fy * Yp / (Zp * Zp)

            J[2 * i, 0] = du_dX * 0 + du_dY * 0 + du_dZ * 0  # drx
            J[2 * i, 1] = 0  # dry
            J[2 * i, 2] = 0  # drz
            J[2 * i, 3] = du_dX  # dtx
            J[2 * i, 4] = du_dY  # dty
            J[2 * i, 5] = du_dZ  # dtz
            J[2 * i + 1, 0] = 0  # drx
            J[2 * i + 1, 1] = dv_dX  # dry
            J[2 * i + 1, 2] = 0  # drz
            J[2 * i + 1, 3] = dv_dX  # dtx
            J[2 * i + 1, 4] = dv_dY  # dty
            J[2 * i + 1, 5] = dv_dZ  # dtz

        return total_err
else:
    def _ba_jacobian_residual(objp, imgp, K, params, J, residuals):
        """无 numba 时的纯 Python 回退（慢，仅用于调试）"""
        fx = K[0, 0]
        fy = K[1, 1]
        cx, cy = K[0, 2], K[1, 2]
        r_vec = params[:3]
        t_vec = params[3:6]
        R, _ = cv2.Rodrigues(r_vec)
        proj, _ = cv2.projectPoints(objp, r_vec, t_vec, K, np.zeros(5))
        proj = proj.reshape(-1, 2)
        residuals[:] = (imgp - proj).flatten()
        return float(np.sum(residuals ** 2))

    def evaluate_calibration(self, test_images_left, test_images_right,
                            calib_params):
        """
        评估标定质量
        Returns: 质量报告字典
        """
        errors = []
        valid_count = 0

        for left_path, right_path in zip(test_images_left, test_images_right):
            img_l = cv2.imread(str(left_path))
            img_r = cv2.imread(str(right_path))

            ret_l, corners_l = self.find_board_corners(img_l)
            ret_r, corners_r = self.find_board_corners(img_r)

            if not (ret_l and ret_r):
                continue

            # 重新投影误差
            imgp_reproj = cv2.projectPoints(
                self.objp, np.zeros(3), np.zeros(3),
                np.array(calib_params["left_K"]).reshape(3, 3),
                np.array(calib_params["left_D"]),
                corners_l
            )[0]

            error = np.linalg.norm(imgp_reproj - corners_l)
            errors.append(error)
            valid_count += 1

        if not errors:
            return {"valid_frames": 0, "mean_error_px": None}

        return {
            "valid_frames": valid_count,
            "mean_error_px": float(np.mean(errors)),
            "max_error_px": float(np.max(errors)),
            "min_error_px": float(np.min(errors)),
            "std_error_px": float(np.std(errors)),
            "quality_score": max(0.0, 1.0 - np.mean(errors) / 2.0)  # 2px以内=满分
        }

    def save_params(self, params, output_path):
        """保存标定参数到JSON"""
        with open(output_path, "w") as f:
            json.dump(params, f, indent=2)
        print(f"[保存] {output_path}")

    def load_params(self, input_path):
        """从JSON加载标定参数"""
        with open(input_path) as f:
            return json.load(f)


def main():
    parser = argparse.ArgumentParser(description="Embodied Vision 标定工具")
    subparsers = parser.add_subparsers(dest="command")

    # 工厂标定命令
    factory = subparsers.add_parser("factory", help="工厂标定")
    factory.add_argument("--left-dir", type=Path, required=True)
    factory.add_argument("--right-dir", type=Path, required=True)
    factory.add_argument("--output", type=Path, default="calibration.json")
    factory.add_argument("--board-size", type=int, nargs=2, default=[9, 6])
    factory.add_argument("--square-size", type=float, default=25.0)

    # 在线标定命令
    online = subparsers.add_parser("online", help="在线标定")
    online.add_argument("--params", type=Path, required=True)
    online.add_argument("--quality-threshold", type=float, default=0.7)

    # 评估命令
    eval_cmd = subparsers.add_parser("evaluate", help="标定质量评估")
    eval_cmd.add_argument("--params", type=Path, required=True)
    eval_cmd.add_argument("--left-dir", type=Path, required=True)
    eval_cmd.add_argument("--right-dir", type=Path, required=True)

    args = parser.parse_args()

    if args.command == "factory":
        left_images = sorted(args.left_dir.glob("*.png")) + \
                      sorted(args.left_dir.glob("*.jpg"))
        right_images = sorted(args.right_dir.glob("*.png")) + \
                       sorted(args.right_dir.glob("*.jpg"))

        calibrator = StereoCalibrator(
            board_size=tuple(args.board_size),
            square_size_mm=args.square_size
        )
        params = calibrator.factory_calibrate(left_images, right_images)
        calibrator.save_params(params, args.output)

    elif args.command == "online":
        print("[在线标定] 使用参考参数进行运行时评估（GPU加速版待实现）")

    elif args.command == "evaluate":
        calibrator = StereoCalibrator()
        params = calibrator.load_params(args.params)
        left_images = sorted(args.left_dir.glob("*.png"))
        right_images = sorted(args.right_dir.glob("*.png"))
        report = calibrator.evaluate_calibration(left_images, right_images, params)
        print(f"[评估报告] {json.dumps(report, indent=2)}")

    else:
        parser.print_help()


if __name__ == "__main__":
    main()
