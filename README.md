# PAD Camera Resolution Probe

这是一个基于 `Qt 5.15.2 + MSVC 2019 64-bit + CMake + C++11` 的最小 Demo 框架，用来验证：

- Qt 能不能识别出后置摄像头
- Qt 能返回哪些预览分辨率（`viewfinder`）
- Qt 能返回哪些拍照分辨率（`image capture`）
- 实际拍出来的照片到底是不是高分辨率

## 当前版本包含

- 摄像头枚举
- 默认优先选中后置摄像头
- 打开/停止摄像头
- 预览窗口
- Qt 能力列表展示
- 预览档位 / 拍照档位选择
- 一键应用最高档
- 拍照保存到系统图片目录
- 运行日志

## 目录结构

```text
.
├─ CMakeLists.txt
├─ README.md
└─ src
   ├─ MainWindow.cpp
   ├─ MainWindow.h
   └─ main.cpp
```

## 构建方式

如果你的 Qt 安装路径是 `C:/Qt/5.15.2/msvc2019_64`，可以直接这样生成：

```powershell
cmake -S . -B build -G "Visual Studio 16 2019" -A x64 -DCMAKE_PREFIX_PATH=C:/Qt/5.15.2/msvc2019_64
cmake --build build --config Release
```

## 运行后怎么测

1. 启动程序
2. 看顶部是否默认选中了后置摄像头
3. 点击“打开所选”
4. 观察右侧：
   - `预览(Viewfinder)列表`
   - `拍照(Capture)分辨率列表`
5. 点击“应用最高档”
6. 点击“拍照”
7. 看左下角“最近照片”的实际尺寸

## 你要重点看什么

- 如果 `预览探测帧` 很低，但 `最近照片` 很高，说明：
  - Qt 预览流分辨率低
  - 但拍照流可能能到更高分辨率
- 如果 `预览列表` 和 `拍照列表` 本身都没有最高分辨率，那就说明：
  - Qt 当前后端没有把那个更高档位暴露出来
  - 这正是你想验证的点
