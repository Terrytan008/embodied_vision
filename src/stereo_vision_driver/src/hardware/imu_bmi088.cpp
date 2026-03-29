// 硬件抽象层 — BMI088 IMU 驱动
// imu_bmi088.cpp

#include "stereo_vision/hardware/camera_types.hpp"

#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/i2c-dev.h>
#include <linux/i2c.h>
#include <errno.h>
#include <cmath>
#include <cstring>

namespace stereo_vision::hardware {

// ============================================================================
// BMI088 IMU 寄存器定义
// ============================================================================
namespace BMI088Reg {

// 通用
constexpr uint8_t CHIP_ID = 0x00;
constexpr uint8_t ERR_REG = 0x02;

// 加速度计
constexpr uint8_t ACC_CHIP_ID = 0x00;
constexpr uint8_t ACC_PWR_CTRL = 0x7D;
constexpr uint8_t ACC_PWR_CONF = 0x7C;
constexpr uint8_t ACC_CONF = 0x40;
constexpr uint8_t ACC_RANGE = 0x41;
constexpr uint8_t ACCEL_X_LSB = 0x12;
constexpr uint8_t ACCEL_X_MSB = 0x13;
constexpr uint8_t ACCEL_Y_LSB = 0x14;
constexpr uint8_t ACCEL_Y_MSB = 0x15;
constexpr uint8_t ACCEL_Z_LSB = 0x16;
constexpr uint8_t ACCEL_Z_MSB = 0x17;
constexpr uint8_t ACCEL_TEMP_LSB = 0x1D;  // 温度
constexpr uint8_t ACCEL_TEMP_MSB = 0x1E;

// 陀螺仪
constexpr uint8_t GYRO_CHIP_ID = 0x00;
constexpr uint8_t GYRO_PWR_CONF = 0x11;
constexpr uint8_t GYRO_PWR_CTRL = 0x15;
constexpr uint8_t GYRO_RANGE = 0x0B;
constexpr uint8_t GYRO_CONF = 0x0F;
constexpr uint8_t GYRO_X_LSB = 0x02;
constexpr uint8_t GYRO_X_MSB = 0x03;
constexpr uint8_t GYRO_Y_LSB = 0x04;
constexpr uint8_t GYRO_Y_MSB = 0x05;
constexpr uint8_t GYRO_Z_LSB = 0x06;
constexpr uint8_t GYRO_Z_MSB = 0x07;

// 常用值
constexpr uint8_t ACC_CHIP_ID_VAL = 0x1E;  // BMI088加速度计ID
constexpr uint8_t GYRO_CHIP_ID_VAL = 0x0F;  // BMI088陀螺仪ID

}  // namespace BMI088Reg

// ============================================================================
// BMI088 IMU 驱动
// ============================================================================
class BMI088Driver {
public:
    BMI088Driver();
    ~BMI088Driver();

    /**
     * @brief 打开 I2C 总线
     * @param i2c_bus /dev/i2c-X 路径
     * @param addr I2C地址 (ACC=0x18或0x19, GYRO=0x68或0x69)
     */
    bool open(const char* i2c_bus, uint8_t acc_addr, uint8_t gyro_addr);

    /** @brief 初始化 IMU */
    bool init();

    /** @brief 读取 IMU 数据 */
    bool read(IMURawData& data);

    /** @brief 关闭设备 */
    void close();

    /** @brief 检查是否已打开 */
    bool isOpen() const { return fd_ >= 0; }

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

// --------------------------------------------------------------------------
// 实现
// --------------------------------------------------------------------------

BMI088Driver::BMI088Driver() = default;

BMI088Driver::~BMI088Driver() {
    close();
}

bool BMI088Driver::open(const char* i2c_bus, uint8_t acc_addr, uint8_t gyro_addr) {
    fd_ = ::open(i2c_bus, O_RDWR);
    if (fd_ < 0) {
        return false;
    }

    accel_addr_ = acc_addr;
    gyro_addr_ = gyro_addr;

    // 设置 I2C 地址
    if (::ioctl(fd_, I2C_SLAVE, accel_addr_) < 0) {
        ::close(fd_);
        fd_ = -1;
        return false;
    }

    return true;
}

bool BMI088Driver::init() {
    if (fd_ < 0) return false;

    // 1. 软复位
    if (!writeReg(accel_addr_, 0x7E, 0xB6)) return false;
    usleep(10000);  // 10ms

    // 2. 检查加速度计 CHIP_ID
    uint8_t chip_id = 0;
    if (!readReg(accel_addr_, BMI088Reg::ACC_CHIP_ID, &chip_id)) return false;
    if (chip_id != BMI088Reg::ACC_CHIP_ID_VAL) {
        return false;  // ID不匹配
    }

    // 3. 配置加速度计
    if (!configAccelerometer()) return false;

    // 4. 检查陀螺仪 CHIP_ID
    if (!readReg(gyro_addr_, BMI088Reg::GYRO_CHIP_ID, &chip_id)) return false;
    if (chip_id != BMI088Reg::GYRO_CHIP_ID_VAL) {
        return false;
    }

    // 5. 配置陀螺仪
    if (!configGyroscope()) return false;

    return true;
}

bool BMI088Driver::configAccelerometer() {
    // 启动加速度计
    if (!writeReg(accel_addr_, BMI088Reg::ACC_PWR_CONF, 0x00)) return false;
    usleep(5000);
    if (!writeReg(accel_addr_, BMI088Reg::ACC_PWR_CTRL, 0x04)) return false;  // ACC_ON
    usleep(10000);

    // 设置数据率: 1600Hz / ODR=200Hz (Divider=7)
    if (!writeReg(accel_addr_, BMI088Reg::ACC_CONF, 0xA7)) return false;

    // 设置量程: ±16g (0x03)
    if (!writeReg(accel_addr_, BMI088Reg::ACC_RANGE, 0x03)) return false;
    accel_lsb_mg_ = 0.061f;  // ±16g

    // 内部滤波器: BW=ODR/2
    return true;
}

bool BMI088Driver::configGyroscope() {
    // 启动陀螺仪
    if (!writeReg(gyro_addr_, BMI088Reg::GYRO_PWR_CTRL, 0x02)) return false;  // GYRO_ON
    usleep(5000);

    // 设置数据率: 200Hz (Gyro output rate=3200Hz, div=15)
    if (!writeReg(gyro_addr_, BMI088Reg::GYRO_CONF, 0xA7)) return false;

    // 设置量程: ±2000 dps (0x02)
    if (!writeReg(gyro_addr_, BMI088Reg::GYRO_RANGE, 0x02)) return false;
    gyro_lsb_dps_ = 16.384f;  // ±2000 dps

    return true;
}

bool BMI088Driver::read(IMURawData& data) {
    if (fd_ < 0) return false;

    // 读取加速度计 (7字节: x,y,z + temp)
    uint8_t buf[7] = {0};
    if (!readRegs(accel_addr_, BMI088Reg::ACCEL_X_LSB, buf, 7)) return false;

    // 检查数据是否就绪 (bit 0 of x LSB = 0)
    // BMI088 数据格式: 16bit signed, MSB first, LSB first
    int16_t acc_x = static_cast<int16_t>((buf[1] << 8) | buf[0]);
    int16_t acc_y = static_cast<int16_t>((buf[3] << 8) | buf[2]);
    int16_t acc_z = static_cast<int16_t>((buf[5] << 8) | buf[4]);

    // 温度 (0.125°C/LSB, offset -23°C)
    int16_t temp_raw = static_cast<int16_t>((buf[6] << 8) | (buf[6] & 0xFF));

    // 读取陀螺仪
    if (!readRegs(gyro_addr_, BMI088Reg::GYRO_X_LSB, buf, 6)) return false;

    int16_t gyro_x = static_cast<int16_t>((buf[1] << 8) | buf[0]);
    int16_t gyro_y = static_cast<int16_t>((buf[3] << 8) | buf[2]);
    int16_t gyro_z = static_cast<int16_t>((buf[5] << 8) | buf[4]);

    // 填充原始数据
    data.gyro[0] = gyro_x;
    data.gyro[1] = gyro_y;
    data.gyro[2] = gyro_z;
    data.accel[0] = acc_x;
    data.accel[1] = acc_y;
    data.accel[2] = acc_z;
    data.temp = temp_raw;
    data.timestamp_ns = 0;  // 由调用方填充
    data.valid = true;

    return true;
}

void BMI088Driver::close() {
    if (fd_ >= 0) {
        // 关闭加速度计
        writeReg(accel_addr_, BMI088Reg::ACC_PWR_CONF, 0x03);  // suspend
        // 关闭陀螺仪
        writeReg(gyro_addr_, BMI088Reg::GYRO_PWR_CTRL, 0x00);  // off
        ::close(fd_);
        fd_ = -1;
    }
}

bool BMI088Driver::writeReg(uint8_t addr, uint8_t reg, uint8_t value) {
    if (fd_ < 0) return false;

    // 切换 I2C 地址
    if (::ioctl(fd_, I2C_SLAVE, addr) < 0) return false;

    uint8_t buf[2] = {reg, value};
    if (::write(fd_, buf, 2) != 2) return false;
    return true;
}

bool BMI088Driver::readReg(uint8_t addr, uint8_t reg, uint8_t* value) {
    if (fd_ < 0) return false;
    if (::ioctl(fd_, I2C_SLAVE, addr) < 0) return false;

    // 写寄存器地址
    if (::write(fd_, &reg, 1) != 1) return false;

    // 读取值
    if (::read(fd_, value, 1) != 1) return false;
    return true;
}

bool BMI088Driver::readRegs(uint8_t addr, uint8_t reg, uint8_t* buffer, size_t len) {
    if (fd_ < 0) return false;
    if (::ioctl(fd_, I2C_SLAVE, addr) < 0) return false;

    if (::write(fd_, &reg, 1) != 1) return false;
    if (::read(fd_, buffer, len) != static_cast<ssize_t>(len)) return false;
    return true;
}

float BMI088Driver::gyroRawToDps(int16_t raw) {
    return raw * gyro_lsb_dps_ / 32768.0f;
}

float BMI088Driver::accelRawToMg(int16_t raw) {
    return raw * accel_lsb_mg_;
}

}  // namespace stereo_vision::hardware
