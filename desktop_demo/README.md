# 断点萌宠：电脑端软件原型

这一版不依赖主控板、摄像头或语音板，用浏览器按钮和文字输入模拟硬件事件，先验证完整业务流程。它是开发和联调工具，不是最终参赛作品；最终版本必须迁移到“灵机一动”真实设备上运行：

1. 检测到用户准备离座；
2. 询问是否保存断点；
3. 用户通过语音内容记录当前进度；
4. SQLite 本地保存；
5. 检测到用户归来；
6. 主动播报最近一个未完成断点；
7. 用户可以随时说“记录断点”手动保存。

## 运行

```powershell
cd desktop_demo
python app.py
```

浏览器打开：<http://127.0.0.1:5000>

注意：本地 Flask 开发服务器默认只支持 `http://`，不要输入 `https://`。浏览器显示“未使用安全连接”是正常的，因为页面只运行在本机 `127.0.0.1`，不会发送到公网。

当前电脑已经安装 Flask 3.1.3。换电脑时可以执行：

```powershell
python -m pip install -r requirements.txt
```

## 测试

```powershell
python -m unittest discover -s tests -v
```

## 目录

```text
breakpoint_pet/
├─ app.py                 Flask API 与调试页面
├─ pet/
│  ├─ controller.py      对话和状态机
│  ├─ models.py          状态与断点数据结构
│  └─ storage.py         SQLite 本地存储
├─ templates/index.html  调试页面
├─ static/app.js         页面交互
├─ static/style.css      页面样式
├─ tests/                自动化测试
└─ data/                 运行后自动生成数据库
```

## 将来接入硬件

- 摄像头姿态模块检测到起身趋势时，调用 `controller.pre_leave_detected()`。
- 摄像头检测到用户重新坐下时，调用 `controller.return_detected()`。
- 语音识别结果传给 `controller.receive_voice(text)`。
- `ControllerResponse.speech` 交给语音板播放。
- 灯板和舵机根据 `ControllerResponse.state` 执行动作。

核心状态机与具体硬件接口分离，因此硬件到达后不需要重写业务逻辑。

The hardware implementation is in `../firmware/apps/main`.


