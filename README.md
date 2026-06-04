# d3d11 — Win32 屏幕捕获与 GPU 渲染 DLL

基于 **D3D11 + DXGI + Windows Graphics Capture (WGC)** 的 Windows 动态库，输出 **BGRA** 帧数据，并可将帧 **GPU/GDI 绘制到指定窗口**。提供 C 导出接口（`__stdcall`），便于易语言等宿主调用。

| 项目 | 说明 |
|------|------|
| 产物 | `d3d.dll`（Win32，Release） |
| 系统 | Windows 10+，需安装图形栈 |
| 工程 | Visual Studio 2022+，`d3d11.vcxproj` |
| 远程 | [github.com/binggeer/d3d11](https://github.com/binggeer/d3d11) |

---

## 功能概览

- **桌面捕获**：句柄 `0`，优先 **DXGI 桌面复制**（无系统黄框），失败时回退 WGC
- **窗口捕获**：WGC `CreateForWindow`，Win11 可尝试关闭截图黄框（`IsBorderRequired`）
- **区域裁剪**：`cropX/Y/W/H`，宽或高 ≤0 表示全幅
- **截帧**：纹理（GPU）或 CPU 字节集（BGRA，`width × height × 4`）
- **渲染**：`D3D_RenderToWindow` — DXGI 交换链全屏三角缩放，失败自动 **GDI StretchDIBits**
- **外部投屏**：`D3D_SetRenderSize` 仅建渲染设备（scrcpy 等），勿对显示窗做 `Init` 截屏
- **编码探测**：`D3D_GetEncoderInfo` 返回 CPU / HW(NVIDIA|AMD|Intel|h264) 等说明（不实际编码）

---

## 编译

```bash
# 在项目根目录，使用 MSBuild（路径按本机 VS 安装调整）
MSBuild d3d11.vcxproj /p:Configuration=Release /p:Platform=Win32
```

生成：`Release\d3d.dll`

依赖系统 DLL（一般 Win10 已自带）：`d3d11.dll`、`dxgi.dll`、`d3dcompiler_47.dll` 等。

> 本仓库 **不包含** 编译好的 `d3d.dll`（见 `.gitignore`）。克隆后需自行编译。

---

## 快速开始（C / DLL 命令）

### 1. 桌面截屏

```text
D3D_Init(0, 0, 0, 0, 0, 0)          // 源句柄 0 = 桌面，crop 全 0 = 全屏
need = D3D_GetCaptureImageByteCount()
// 分配 need 字节缓冲区
D3D_CaptureToImageBytes(buf, need)
D3D_RenderToWindow(displayHwnd, buf, need)   // 或传 0,0 用内部最后一帧
D3D_Shutdown()
```

### 2. 指定窗口截屏

```text
D3D_Init(sourceHwnd, 0, cropX, cropY, cropW, cropH)
// 其余同上；运行时换窗/区域用 D3D_SetSource
```

### 3. scrcpy / 外部解码投屏（只渲染、不截屏）

```text
D3D_SetRenderSize(frameW, frameH)     // 勿对显示窗口 D3D_Init
D3D_RenderToWindow(displayHwnd, frameBuf, frameW * frameH * 4)
D3D_Shutdown()
```

程序退出或不再使用时 **必须** 调用 `D3D_Shutdown()`，不要仅依赖 DLL 卸载。

---

## 导出 API 一览

| 函数 | 说明 |
|------|------|
| `D3D_Init` | 初始化捕获；返回 0 成功 |
| `D3D_SetSource` | 已初始化后更换窗口/裁剪区 |
| `D3D_SetRenderSize` | 仅渲染模式（外部 BGRA 尺寸） |
| `D3D_Shutdown` | 释放设备与会话 |
| `D3D_GetCaptureWidth/Height` | 当前帧宽高 |
| `D3D_GetCaptureImageByteCount` | 单帧 BGRA 字节数 |
| `D3D_CaptureToTexture` | 截一帧到内部纹理，返回纹理指针 |
| `D3D_CaptureToImageBytes` | 截一帧到 CPU 缓冲区 |
| `D3D_RenderToWindow` | 绘制 BGRA 到目标窗口 |
| `D3D_RenderBlackToWindow` | 目标窗口清黑 |
| `D3D_ReleaseLastFrame` | 释放缓存帧，不关设备 |
| `D3D_GetEncoderInfo` / `D3D_GetLastError` | 编码说明 / 最近错误文本 |
| `D3D_GetDevicePtr` / `GetContextPtr` / `GetTexturePtr` | 高级互操作 |

完整声明见 [`d3d_exports.h`](d3d_exports.h)、[`d3d.def`](d3d.def)。

### 常见错误码

| 返回值 | 含义 |
|--------|------|
| `0` | 成功 |
| `-1` | 未初始化 |
| `-3` | 设备失败 |
| `-5` | 尺寸非法 |
| `-9` | 未找到输出/窗口无效 |
| `-10` | DuplicateOutput（复制被占用） |
| `-11` | 裁剪区域无效 |
| `-13` | 渲染失败 |

---

## 易语言封装

仓库内 **不提交** 易语言源码（`类_D3D11.txt`、`DLL.txt` 等在 `.gitignore`）。本地可参考类方法：

| 类方法 | DLL |
|--------|-----|
| 初始化 | `D3D_Init` |
| 置区域 | `D3D_SetSource` |
| 置渲染尺寸 | `D3D_SetRenderSize` |
| 截屏到字节集 | `D3D_CaptureToImageBytes` |
| 渲染到窗口 | `D3D_RenderToWindow` |
| 销毁 | `D3D_Shutdown` |

使用前在易语言中 **置 DLL 装载目录** 指向含 `d3d.dll` 的 `Release` 目录。

---

## 截图黄框说明

| 场景 | 行为 |
|------|------|
| **桌面** | 优先 DXGI 复制，通常 **无黄框** |
| **窗口** | WGC；Win11 可设 `IsBorderRequired(false)`，并可在系统设置中允许「关闭截图边框」 |
| **Win10 窗口** | 系统限制，黄框可能无法关闭 |
| 复制失败回退 WGC | 桌面也可能出现黄框，需关闭占用 DXGI 的其他软件 |

---

## 仓库结构

```text
d3d11/
├── d3d_core.cpp      # 捕获/渲染核心
├── d3d_api.cpp       # C 导出与异常保护
├── d3d_exports.h     # 头文件声明
├── d3d_internal.h
├── d3d.def
├── d3d11.vcxproj
├── README.md         # 本说明
└── 推送到GitHub.md   # Git 推送步骤
```

---

## 系统权限与依赖

- 首次 **窗口 WGC** 捕获：请在 **设置 → 隐私 → 屏幕截图** 中允许本程序。
- Win11 去黄框：可开启 **允许应用关闭截图边框**。
- `d3d11` / `dxgi` 无法静态合并进本 DLL，需目标机已安装正常图形驱动。

---

## 参与与推送

修改代码、提交并推送到 GitHub 的步骤见 **[推送到GitHub.md](推送到GitHub.md)**。

---

## 许可

若仓库未单独声明 License，使用前请与仓库维护者确认分发与商用条款。
