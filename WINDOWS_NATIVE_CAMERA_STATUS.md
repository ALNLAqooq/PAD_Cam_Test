# Windows Native Camera 开发记录

更新时间：2026-03-10

当前工作分支：

- `feat/windows-native-camera-focus`

## 1. 目标

这个项目的目标是：

- 保留现有 Qt 界面
- 将相机后端从单一的 `Qt Multimedia` 扩展为“可切换后端”
- 在 Windows PAD / 普通 UVC webcam 场景下，逐步实现：
  - 原生设备枚举
  - 原生预览
  - 原生分辨率 / 档位切换
  - 原生拍照
  - 最终的点击对焦 / ROI 对焦

## 2. 当前已完成内容

### 2.1 后端抽象完成

新增统一后端接口：

- `src/CameraBackend.h`
- `src/CameraBackend.cpp`

当前后端结构：

- `QtCameraBackend`
  - 保留原先基于 Qt Multimedia 的能力
- `WindowsNativeCameraBackend`
  - 用于承接 Windows 原生相机实现

主界面已经切换为通过后端接口驱动，而不是直接耦合 `QCamera`。

相关文件：

- `src/MainWindow.h`
- `src/MainWindow.cpp`

### 2.2 Qt 后端保留可用

`QtCameraBackend` 已完成迁移，保留如下功能：

- 枚举设备
- 打开 / 停止相机
- 预览
- 档位切换
- 拍照
- 焦点能力检测日志

相关文件：

- `src/QtCameraBackend.h`
- `src/QtCameraBackend.cpp`

### 2.3 Windows Native 后端已完成的能力

当前 `WindowsNativeCameraBackend` 已实现：

- Windows 原生设备枚举
  - 基于 `Media Foundation`
- 基础原生预览
  - 基于 `IMFSourceReader`
- 原生预览档位枚举
- 预览档位应用
- 基础拍照
  - 当前实现方式：保存最近一帧预览图像
  - 不是独立原生拍照管线

相关文件：

- `src/WindowsNativeCameraBackend.h`
- `src/WindowsNativeCameraBackend.cpp`

### 2.4 界面布局优化

已经修复右侧面板过宽的问题，主要调整如下：

- 左侧预览优先占空间
- 右侧面板限制宽度
- 防止 `splitter` 子面板塌缩

相关文件：

- `src/MainWindow.cpp`

## 3. 当前技术路线结论

### 3.1 已验证可行的路线

当前已经验证可用：

- `Media Foundation + SourceReader`

这条路线适合：

- 设备枚举
- 预览
- 抓帧
- 基础拍照

### 3.2 未最终落地的路线

目标中的“点击对焦 / ROI 对焦”暂未继续往下实现。

原因不是界面层不会做，而是：

- 真正的点击对焦更接近 Windows Camera / WinRT 相机控制栈
- 当前这条 `Media Foundation + SourceReader` 路线虽然适合预览和抓帧，但不确定是否适合作为最终 ROI 对焦控制入口

当前判断：

- 如果只是预览 / 拍照：当前原生路线可继续用
- 如果要实现“像微软相机那样点哪里对哪里”：
  - 更推荐后续转向 `MediaCapture / WinRT / C++/WinRT`

## 4. 当前遇到的问题

### 4.1 Qt 路线无法检测出点击对焦能力

在 PAD 上，Qt 后端日志显示：

- 对焦接口不可用
- 自动对焦 / 连续对焦 / 中心对焦 / 自定义对焦点均不支持

这说明：

- 不是设备一定不支持对焦
- 更可能是 `Qt 5 + DirectShow/Qt Multimedia` 这条链路没有把 webcam 的原生对焦能力暴露出来

### 4.2 开发环境缺少 WinRT 所需 SDK

当前机器检查结果显示，缺少典型的 Windows 10 SDK 路径，例如：

- `C:\Program Files (x86)\Windows Kits\10\Include`
- `C:\Program Files (x86)\Windows Kits\10\UnionMetadata`
- `C:\Program Files (x86)\Windows Kits\10\References`

同时也没有找到明确可用的：

- `C++/WinRT` 头
- 完整可用的 `Windows 10/11 SDK` 元数据环境

这意味着：

- Windows 原生点击对焦的更合适路线（`MediaCapture / WinRT`）当前无法直接继续开发

### 4.3 当前机器安装环境受限

用户当前设备上存在问题：

- 暂时无法方便地通过 Visual Studio Installer 安装/更新所需开发组件

这也是本次暂停的直接原因。

## 5. 如果下次继续，建议优先做什么

### 5.1 先补开发环境

建议优先补齐：

- Windows 10/11 SDK
  - 建议 `10.0.19041` 或更高
- C++/WinRT 支持
- 如有需要，补充 `WindowsApp.lib` / WinRT 相关元数据

### 5.2 再做技术路线切换判断

后续建议按下面顺序推进：

1. 确认 SDK 环境齐全
2. 判断是否将“点击对焦”部分切换到：
   - `MediaCapture / WinRT`
3. 如果切换：
   - 保留现有 Qt 界面
   - 原生后端改为更接近 Windows Camera 的控制栈

### 5.3 如果不切 WinRT，当前也还能继续做的事

即使暂时不做点击对焦，当前 `WindowsNativeCameraBackend` 这条线还可以继续增强：

- 优化预览稳定性
- 优化拍照质量
- 真正独立拍照管线
- 更完整的原生分辨率控制

## 6. 本次涉及的主要文件

- `CMakeLists.txt`
- `src/CameraBackend.h`
- `src/CameraBackend.cpp`
- `src/MainWindow.h`
- `src/MainWindow.cpp`
- `src/QtCameraBackend.h`
- `src/QtCameraBackend.cpp`
- `src/WindowsNativeCameraBackend.h`
- `src/WindowsNativeCameraBackend.cpp`

## 7. 当前状态一句话总结

当前项目已经完成：

- Qt 界面 + 可切换后端架构
- Windows 原生后端的设备枚举 / 预览 / 档位 / 基础拍照

当前暂停点在：

- 点击对焦 / ROI 对焦

主要阻塞原因是：

- 更适合实现点击对焦的 `WinRT / MediaCapture` 开发环境当前不完整，需后续补齐 SDK 后继续。
