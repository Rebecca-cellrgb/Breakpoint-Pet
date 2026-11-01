# 续点桌宠：姿态模型测试集

这里不是训练集，而是一组用于快速排查的测试样本。目标是先回答三个问题：

1. ONNX 模型能否正常加载并输出三组姿态特征；
2. 普通坐姿、使用电脑和局部人体画面中，17 个人体关键点能否被检测到；
3. 后续在开发板上运行 `.espdl` 时，结果是否与电脑端大致一致。

## 数据来源

- `Bingsu/Human_Action_Recognition`：抽取 `sitting` 和 `using_laptop` 两类，各 12 张。该数据集页面标注为 ODbL。
  <https://huggingface.co/datasets/Bingsu/Human_Action_Recognition>
- `vrg-prague/CropCOCO`：抽取 12 张裁剪人体图片，用于观察人体不完整时的关键点表现。该数据集页面标注为 GPL-3.0。
  <https://huggingface.co/datasets/vrg-prague/CropCOCO>

这些图片仅用于本项目的模型测试。提交作品或重新分发图片前，应再次核对源数据集页面的许可证和署名要求。

## 下载

在 PowerShell 中执行：

```powershell
cd firmware\pose_test
python .\download_samples.py
```

脚本只使用 Python 标准库，输出到 `pose_test/data/`，并生成 `pose_test/manifest.csv`。

## 为什么还要自拍数据

公开图片能测试“单张图里能否找到人和关键点”，但不能证明“坐下 → 起身 → 离开 → 回来”的状态机正确。最终还需固定桌宠摄像头机位，自行拍摄至少：

- 坐着 10 段；
- 起身但未离开 10 段；
- 离开画面 10 段；
- 返回并坐下 10 段；
- 弯腰捡东西、转身、路人经过等容易误判的反例各 5 段。

自拍视频只需用于测试，第一版不需要拿来训练模型。

## 电脑端运行 ONNX 测试

请在本机准备 Python 依赖；虚拟环境不纳入提交目录：

```powershell
cd firmware\pose_test
python -m pip install onnxruntime numpy pillow
python .\run_onnx_test.py
```

输出文件：

- `pose_test/results.csv`：每张图片的人体数量、最高置信度、可见关键点数和推理耗时；
- `pose_test/results/`：画有检测框和骨架的图片。

本次实测结果（Windows CPU，2026-10-22）：

| 场景 | 检出 | 平均最高置信度 | 平均可见关键点 | 平均 ONNX 推理耗时 |
|---|---:|---:|---:|---:|
| sitting | 12/12 | 0.82 | 12.8/17 | 27.9 ms |
| using_laptop | 12/12 | 0.80 | 10.0/17 | 26.5 ms |
| CropCOCO 局部人体 | 8/12 | 0.84 | 11.0/17 | 31.6 ms |

这能证明仓库内 ONNX 模型可加载、可推理，且普通坐姿下的关键点有效；不能单独证明量化后的 `.espdl` 在开发板上完全一致，也不能证明“起身/离开/返回”的时间状态机已经正确。后二者仍需等硬件到手后，用固定机位自拍视频验证。

## 第二轮：坐下与站起视频

为了测试连续动作，从 `divm/hmdb51` 的公开测试集只抽取了 10 段 `sit` 和 10 段 `stand` MP4。HMDB51 包含 51 类、6,766 段视频；该镜像说明数据仅供研究使用，正式参赛展示时应保留数据集引用。

数据集：<https://huggingface.co/datasets/divm/hmdb51>

运行：

```powershell
cd firmware\pose_test
python .\download_hmdb51_motion_samples.py
python .\run_motion_test.py
```

测试器每段均匀抽取 15 帧，统计人体检出率、关键点完整度，以及人体框纵横比、膝关节角度和大腿竖直程度从片头到片尾的变化方向。

| 动作 | 平均逐帧人体检出率 | 平均可见关键点 | 可判断视频中的方向通过数 |
|---|---:|---:|---:|
| sit down | 100.0% | 10.2/17 | 7/8 |
| stand up | 92.7% | 10.7/17 | 6/10 |

把关键点原始阈值从仓库默认的 `0.5` 降至 `0.25` 后，平均关键点仅增加约 0.3–0.5 个，方向通过数完全没有提高。因此目前没有证据支持直接降低阈值；这样做更可能引入低置信度错误点。

HMDB51 中若干失败片段包含电影切镜头、面部近景、多人、汽车遮挡或人物只在结尾出现，与固定桌面摄像头差异很大。它适合做压力测试，但不能替代最终的固定机位自拍视频。

输出：

- `motion_results.csv`：每段视频汇总；
- `motion_frame_results.csv`：逐帧测量；
- `motion_results/`：每段视频的开头、中间、结尾骨架故事板；
- `motion_results_kpt025.csv`：阈值 0.25 的对照结果。
