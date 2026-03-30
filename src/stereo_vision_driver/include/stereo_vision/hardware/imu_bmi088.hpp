// BMI088 IMU 驱动接口
// imu_bmi088.hpp

#pragma once

#include "camera_types.hpp"
#include <cstdint>
#include <string>

namespace stereo_vision::hardware {

/**
 * @brief BMI088 IMU 驱动
 *
 * 通过 I2C 访问 Bosch BMI088 惯性测量单元。
 * 实现在 imu_bmi088.cpp。
 */
class BMI088Driver {
public:
    BMI088Driver();
    ~BMI088Driver();

    /**
     * @brief 打开 I2C 总线
     * @param i2c_bus /dev/i2c-X 路径
     * @param acc_addr 加速度计 I2C 地址 (默认 0x18)
     * @param gyro_addr 陀螺仪 I2C 地址 (默认 0x68)
     */
    bool open(const char* i2c_bus, uint8_t acc_addr = 0x18, uint8_t gyro_addr = 0x68);

    /** @brief 初始化 IMU */
    bool init();

    /** @brief 读取 IMU 数据 */
    bool read(IMURawData& data);

    /** @brief 关闭设备 */
    void close();

    /** @brief 检查是否已打开 */
    bool isOpen() const { return fd_ >= 0; }

    /** @brief 获取版本 */
    std::string getVersion() const;

private:
    // I2C 读写
    bool writeReg(uint8_t addr, uint8_t reg, uint8_t value);
    bool readReg(uint8_t addr, uint8_t reg, uint8_t* value);
    bool readRegs(uint8_t addr, uint8_t reg, uint8_t* buffer, size_t len);

    // 配置
    bool configAccelerometer();
    bool configGyroscope();

    // 物理量转换
    float gyroRawToDps(int16_t raw);
    float accelRawToMg(int16_t raw);

    int fd_ = -1;
    uint8_t accel_addr_ = 0x18;
    uint8_t gyro_addr_ = 0x68;

    // 转换系数（初始化时从寄存器读取）
    float gyro_lsb_dps_ = 16.384f;   // ±2000 dps
    float accel_lsb_mg_ = 0.061f;    // ±16g
};

}  // namespace stereo_vision::hardware
