# 断点萌宠

面向学习和办公场景的智能陪伴设备：检测到用户准备离开时询问是否保存任务断点；用户用语音描述进度后本地保存；用户回来时主动播报最近一次未完成的断点。

## 提交内容

本目录已整理为可上传的源码内容，不含 Git 元数据、虚拟环境、编译目录、运行日志、缓存和临时数据库。

```text
breakpoint-pet/
|- desktop_demo/          # 无需硬件即可运行的 Flask 演示和自动化测试
|- firmware/              # ESP-IDF 应用层、外设、语音、云端和姿态代码
|  |- apps/main/           # 固件主程序和业务模块
|  |- apps/sdkconfig.defaults
|  |- apps/partitions.csv
|  `- pose_test/           # 电脑端姿态识别和主板联调脚本
`- README.md
```

## 建议查看顺序

先运行 `desktop_demo` 查看核心流程；硬件端实现位于 `firmware/apps/main`。

- `breakpoint_pet.c`：断点状态机、本地持久化、语音交互和超时处理。
- `peripheral/bsp_camera.c`：摄像头取流、局域网发现和姿态事件接收。
- `peripheral/bsp_servo.c`、`bsp_ws2812.c`、`bsp_vibrator.c`：舵机、灯环和振动马达控制。
- `agent/agent_app.c`、`voice/`、`cloud/`：语音会话、TTS/ASR 和联网业务。
- `pose/`、`pose_test/`：姿态模型封装和电脑端摄像头联调。

## 运行电脑端演示

```powershell
cd desktop_demo
python -m pip install -r requirements.txt
python app.py
```

浏览器打开 `http://127.0.0.1:5000`。运行测试：

```powershell
cd desktop_demo
python -m unittest discover -s tests -v
```

## 固件依赖

`firmware/apps` 包含项目应用层源码，不包含完整 ESP-IDF、QUMIOS、Agora IoT SDK 和其他第三方框架。构建需要在本机配置 ESP-IDF 5.3+、匹配版本的 QUMIOS SDK、Agora IoT SDK 及组件管理器依赖。姿态功能默认启用；已保留 ESP32-S3 和 ESP32-P4 使用的 V1 模型文件，未包含完整 ONNX 训练/导出文件和未使用的模型版本。

功能开关和分区配置见 `firmware/apps/main/qm_config/qm_config.h`、`firmware/apps/sdkconfig.defaults` 和 `firmware/apps/partitions.csv`。摄像头联调说明见 `firmware/pose_test/README.md`。

## 上传前检查

1. 确认源码不含 Wi-Fi 密码、云端密钥、设备证书或私有 SDK 配置。
2. 在 GitHub 创建公开仓库，将本目录中的内容上传。
3. 提交仓库地址，并告知评审从根目录 README 查看项目结构和运行说明。
