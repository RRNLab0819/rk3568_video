# 原厂 AVM 鱼眼/全景矫正 — 调查分析报告

> 日期：2026-05-27
> 目的：调查原厂 AVM 系统的矫正参数和实现方式，评估是否可复用到 rk3568_camera 项目

---

## 1. 原厂矫正文件清单

### 1.1 活标定文件（运行时，位于 /userdata/avm/）

| 文件路径 | 大小 | 描述 |
|---------|------|------|
| `/userdata/avm/cali/calibinfo.lua` | 5,724 B | **当前激活的 4 路摄像头标定** |
| `/userdata/avm/cali/calibinfo.lua_bk` | 5,007 B | 备份 |
| `/userdata/avm/cali/calibinfo.lua_org` | 7,650 B | 原始出厂标定 |
| `/userdata/avm/cali/calibinfo2.lua` | 5,057 B | 2 路标定 |
| `/userdata/avm/cali/calibinfo3.lua` | 5,057 B | 3 路标定 |
| `/userdata/avm/cali/calibinfo4.lua` | 5,007 B | 4 路标定 |
| `/userdata/avm/cali/calibinfo42.lua` | 7,506 B | 4+2 路标定（含 SubCameras） |
| `/userdata/avm/cali/calibinfo6.lua` | 7,835 B | 6 路标定 |
| `/userdata/avm/cali/camera_config4.lua` | 526 B | 4 路摄像头设备配置 |
| `/userdata/avm/cali/camera_config6.lua` | 697 B | 6 路摄像头设备配置 |
| `/userdata/avm/scene/camera_config.lua` | 533 B | **摄像头参数** (1920x1080, 25fps) |
| `/userdata/avm/scene/mesh_param.lua` | 2,098 B | **Mesh 生成参数** (Circle/Sphere/Plane) |
| `/userdata/avm/scene/setting.lua` | 8,856 B | 场景设置 |
| `/userdata/avm/scene/viewport_pos.lua` | 210 B | 视口位置 |

### 1.2 静态/预设文件（只读，位于 /oem/birdview/）

| 文件路径 | 大小 | 描述 |
|---------|------|------|
| `/oem/birdview/camera_module.lua` | 8,183 B | 摄像头矫正顶层模块 |
| `/oem/birdview/scripts/camera_module.lua` | 6,440 B | 摄像头矫正完整模块 |
| `/oem/birdview/scripts/mesh_module.lua` | 6,152 B | Mesh 生成和渲染 |
| `/oem/birdview/scripts/scene_studio.lua` | 38,186 B | SceneStudio 主渲染循环 |
| `/oem/birdview/scripts/render_frame.lua` | 2,584 B | 框架初始化/配置 |
| `/oem/birdview/scripts/geometry_module.lua` | 3,741 B | 几何体渲染 |
| `/oem/birdview/scripts/stream_module.lua` | 2,805 B | 视频流到 mesh 纹理 |
| `/oem/birdview/scripts/lens.lua` | 823,092 B | **镜头畸变查表 (42 个镜头型号)** |
| `/oem/birdview/scripts/opengl.lua` | - | OpenGL 封装 |
| `/oem/birdview/scene/mesh_param_270.lua` | 1,979 B | 270 度 mesh 参数 |

### 1.3 GPU Shader 文件（位于 /oem/birdview/shaders/）

| 文件 | 大小 | 描述 |
|------|------|------|
| `texture_y_uv.frag` | 535 B | **NV12→RGB 着色器** (已复用) |
| `texture_y_u_v.frag` | 635 B | 3-plane YUV |
| `texture_y_vu.frag` | 522 B | NV21 变体 |
| `main_video_vold.frag` | 1,147 B | 主视频着色器（亮度/对比度/饱和度） |
| `main_pos_tex.vert` | 124 B | 顶点着色器（位置+纹理坐标） |
| `mvp.vert` | 135 B | **MVP 矩阵顶点着色器** (mesh 渲染用) |
| `mvp_blend_side.vert` | 398 B | 侧视融合顶点着色器 |
| `near_blend.vert` | 234 B | 近场融合顶点着色器 |
| `paint_lighting.vert` | 724 B | 光照顶点着色器 |
| `main_video_bias.frag` | 630 B | 视频偏置着色器 |
| `texture_rgba.frag` | 190 B | RGBA 纹理着色器 |

### 1.4 C++ 核心库（位于 /oem/birdview/lib/）

| 库文件 | 描述 |
|--------|------|
| `libLuaApi.so.1` | **核心 C API (Camera/Mesh/Math 函数)** |
| `libMath3D.so.1` | 3D 数学库 |
| `libBirdviewFrame.so.1` | Birdview 帧处理 |
| `libBirdviewUtil.so.1` | Birdview 工具 |
| `libGLContext.so.1` / `libGlesContext.so.1` | OpenGL/GLES 上下文 |

---

## 2. 原厂矫正实现方式分析

### 2.1 矫正方法：GPU Mesh/Shader（非 CPU remap，非 RGA）

原厂使用 **逆映射 (Inverse Mapping)** 方法：
1. 在运行时时通过 C++ 函数生成 3D mesh（球面/碗形/平面）
2. 将 3D mesh 顶点通过摄像头内外参投影，得到原始鱼眼图像中的 UV 坐标
3. GPU 通过 shader 采样原始鱼眼纹理，渲染出矫正视图
4. `texture_y_uv.frag` 完成 NV12→RGB 转换

**渲染管线：**
```
3D Mesh 生成 (C++) → 顶点投影 (Camera Model) → UV坐标 → GPU Shader → 矫正画面
                                                      ↑
                                         原始鱼眼 NV12 纹理 (texture0+texture1)
```

### 2.2 每路摄像头独立内外参

**是的。** 每路摄像头有完整且独立的参数：

**内参 (Internal)：**
```lua
Center = { 946.42, 553.02 }     -- 光学中心 (像素)
Focal  = 1449.51                 -- 焦距 (像素)
Scale  = 1.005                   -- 缩放因子
DistorParam = { 13.08, 35.63, -2.26, -4.94 }  -- 4阶多项式畸变参数
Lens   = "6028"                  -- 镜头型号（查 lens.lua）
Sensor = "2053"                  -- 传感器型号
PixelSize = { 1920, 1080 }      -- 图像分辨率
```

**外参 (External)：**
```lua
CameraPos = { 219.22, 250.72, -27.92 }   -- 摄像头 3D 世界坐标 (mm)
CameraUp  = { 0.885, 0.454, -0.103 }     -- 上方向向量
LookatPos = { 343.60, 0, -64.36 }        -- 视线目标点
```

### 2.3 标定文件位置

- **活标定**: `/userdata/avm/cali/calibinfo.lua` (运行时加载)
- **出厂标定**: `/userdata/avm/cali/calibinfo.lua_org`
- **预设模板**: `/oem/birdview/avm_param/cali/`

加载流程：`SceneStudio:UpdateCalibFile(path)` → `Scene_LoadCalib(path)` (C API)

### 2.4 Mesh 生成方式：运行时生成

**不是预生成文件**。Mesh 是在运行时由 C++ 函数生成的：

```lua
-- mesh_module.lua
local MeshGenerators = {
    Circle    = Mesh_GenerateCircle,     -- 碗形 (birdview)
    Rectangle = Mesh_GenerateRectangle,   -- 矩形
    Sphere    = Mesh_GenerateSphere,      -- 球面 (单路矫正)
    Plane     = Mesh_GeneratePlane,       -- 平面 (stream)
    Dome      = Mesh_GenerateDome,        -- 穹顶
    Cylinder  = Mesh_GenerateCylinder,    -- 圆柱
}
```

**单路鱼眼矫正使用 Sphere 生成器：**
```lua
{
    CameraIndex = 2,         -- 摄像头编号
    DivideLevel = 48,        -- 细分级别 (48×48 网格)
    Generator   = "Sphere",  -- 球面
    Radius      = 5000,      -- 半径 5m
    SingleAngle = 2,         -- 单视角角度
}
```

### 2.5 Mesh 顶点格式

通过 `mvp.vert` shader 可知，mesh 顶点包含：
```
attribute vec4 a_position;   -- 3D 世界坐标 (x, y, z, w)
```
经 MVP 矩阵变换 → `gl_Position`

纹理坐标由 `ProcessTexcoord()` 在顶点着色器中动态计算（通过摄像头模型投影）。

### 2.6 Shader NV12 采样方式

```glsl
// texture_y_uv.frag (已复用到我们项目)
uniform sampler2D texture0;  // Y 平面
uniform sampler2D texture1;  // UV 平面 (NV12 交错)

yuv.x = texture2D(texture0, texcoord).r;       // Y
yuv.y = texture2D(texture1, texcoord).g - 0.5; // U (from G channel)
yuv.z = texture2D(texture1, texcoord).a - 0.5; // V (from A channel)
yuv.x -= 0.0625;
rgb = yuv2RgbMat * yuv;  // BT.601
```

### 2.7 关键 API 函数

| 函数 | 用途 |
|------|------|
| `Camera_PixelToWorld(sx, sy, camIdx)` | 像素坐标→世界射线 |
| `Camera_GetCameraCenter(camIdx)` | 获取光心位置 |
| `Camera_GetPixelAngle(camIdx, cx, y)` | 获取某像素的入射角 |
| `Scene_LoadCalib(path)` | 加载标定文件 |
| `Mesh_GenerateSphere(params, data)` | 生成球面 mesh |
| `Mesh_GenerateCircle(params, data)` | 生成碗形 mesh |
| `Mesh_RenderMesh(inst, proj, trans)` | 渲染 mesh |
| `Math_MatrixProject(...)` | 设置投影矩阵 |
| `Math_MatrixTransform(view, mtx, rot)` | 视图矩阵变换 |

### 2.8 镜头畸变模型

`lens.lua` 包含 42 个镜头型号的畸变表。每个表的格式：

```lua
["6028"] = {
    FocalLength = 1469.5,
    DistortionTable = {
        {angle_deg, r_ideal, r_real, error},
        -- 例如: {8.0, 0.205346659, 0.206523858, -0.00570}
        -- 0° ~ 90°+ 每隔 0.5°
    }
}
```

当前使用镜头型号 "6028" + 传感器型号 "2053"。

---

## 3. 可复用内容分析

### 3.1 可直接复用

| 内容 | 来源 | 状态 |
|------|------|------|
| NV12→RGB shader | `texture_y_uv.frag` | **已复用** |
| 镜头畸变表 | `lens.lua` Lens "6028" | 可解析使用 |
| 4 路摄像头内外参 | `calibinfo.lua` Cameras[1..4] | 可解析使用 |
| Mesh 参数 | `mesh_param.lua` Sphere | 可参考 |

### 3.2 不能直接复用

| 内容 | 原因 |
|------|------|
| C++ 核心库 (libLuaApi.so 等) | Lua 绑定架构、无法直接链接 |
| Mesh 生成函数 | 编译在闭源 .so 中 |
| SceneStudio 渲染框架 | 完整的 Lua+C++ 架构，太重 |
| Camera_PixelToWorld 实现 | 闭源 |

### 3.3 需要自己实现的

| 内容 | 说明 |
|------|------|
| 畸变矫正算法 | 基于 lens.lua 数据 + DistorParam 实现 |
| UV mesh 生成 | 根据内外参生成矫正 mesh 顶点 |
| Mesh 渲染管线 | 在 display.c 中从 quad 改成 mesh |

---

## 4. 方案 A：复用原厂标定数据

### 4.1 数据读取

**从 calibinfo.lua 解析：**
```c
// 每路摄像头提取:
typedef struct {
    // 内参
    float center_x, center_y;       // 光心
    float focal;                    // 焦距 (像素)
    float scale;
    float distor[4];               // k1, k2, k3, k4
    char  lens[16];                // 镜头型号 “6028”
    
    // 外参
    float cam_pos[3];              // 世界坐标 (mm)
    float cam_up[3];               // 上向量
    float lookat[3];               // 视线目标
    
    // 标定图案
    float major_pattern[8][2];     // 8个棋盘格角点
} camera_calib_t;
```

**从 lens.lua 解析 "6028" 畸变表：**
- 提取 0°~90°+ 的 `(angle, r_ideal, r_real, error)` 四元组
- 构建查表或拟合多项式

### 4.2 UV Mesh 生成算法

对于单路鱼眼矫正（球面模型）：

```
for each vertex (u, v) in output grid (m x n):
    1. 将输出像素 (u, v) 映射到球面 3D 坐标 (x, y, z)
       - 球面半径 R = 5000mm
       - 根据 FOV 和视角方向计算
    
    2. 将 3D 坐标通过摄像头外参变换到摄像头坐标系
       - 世界→摄像头 = inverse(CameraPos, CameraUp, LookatPos)
    
    3. 使用摄像头内参投影到像素坐标
       - r = focal * tan(θ)   (等距投影或其他模型)
       - 或使用 lens.lua 的 angle→r 映射
    
    4. 应用畸变参数
       - r_distorted = r * (1 + k1*r² + k2*r⁴ + k3*r⁶ + k4*r⁸)
    
    5. 得到原始鱼眼图像中的 UV 坐标
       - u_src = center_x + r_distorted * cos(φ)
       - v_src = center_y + r_distorted * sin(φ)
    
    6. 存储为 mesh 顶点纹理坐标
```

### 4.3 display.c 改造方式

当前 `disp_draw()` 使用硬编码的 4 顶点 quad：

```c
float verts[] = {
    x0, y0,  0.0f, 0.0f,   // 4个顶点，固定UV
    ...
};
glDrawArrays(GL_TRIANGLE_FAN, 0, 4);
```

**改造为 mesh 渲染：**

```c
// 预生成矫正 mesh (4800 顶点 = 48×48 细分 × 2 三角形/格)
typedef struct {
    int   num_vertices;
    float *positions;   // 屏幕空间位置 (x, y)
    float *texcoords;   // 鱼眼图像 UV 坐标
    GLuint vbo_pos;
    GLuint vbo_tex;
} fisheye_mesh_t;

fisheye_mesh_t mesh[4];  // 每路摄像头独立 mesh

// disp_draw() 中：
glBindBuffer(GL_ARRAY_BUFFER, mesh[cam].vbo_pos);
glVertexAttribPointer(loc_pos, 2, GL_FLOAT, GL_FALSE, 0, 0);
glBindBuffer(GL_ARRAY_BUFFER, mesh[cam].vbo_tex);
glVertexAttribPointer(loc_tex, 2, GL_FLOAT, GL_FALSE, 0, 0);
glDrawArrays(GL_TRIANGLES, 0, mesh[cam].num_vertices);
```

### 4.4 风险和工时

| 风险 | 等级 | 缓解 |
|------|------|------|
| calibinfo.lua 格式可能因版本变化 | 低 | 格式简单，Lua table 结构稳定 |
| DistorParam 多项式模型精度不足 | 中 | 可用 lens.lua 查表替代 |
| 球面模型 FOV 与原厂不完全匹配 | 中 | 需调试 SingleAngle/FOV 参数 |
| mesh 顶点数影响性能 | 低 | 48×48=2304 顶点，GPU 完全无压力 |

**预估工时**: 2-3 天 (解析 + mesh 生成 + display.c 集成 + 调参)

---

## 5. 方案 B：自己生成 Fisheye Mesh

### 5.1 需要参数

如果无法复用原厂标定数据（例如镜头换了），需要：

| 参数 | 获取方式 |
|------|---------|
| 焦距 f (像素) | 棋盘格标定 |
| 光心 (cx, cy) | 棋盘格标定 |
| 畸变参数 k1~k4 | 棋盘格标定 (OpenCV) |
| 摄像头安装位置/角度 | 物理测量 或 PnP |
| FOV | 从焦距和传感器尺寸推导 |

### 5.2 棋盘格重新标定流程

```bash
# 1. 采集每路摄像头的棋盘格图像 (9×6 或类似)
# 2. 用 OpenCV 标定
python3 calibrate.py --images cam0_*.jpg --pattern 9x6 --square 25mm

# 输出:
# camera_matrix: [[fx, 0, cx], [0, fy, cy], [0, 0, 1]]
# dist_coeffs: [k1, k2, p1, p2, k3]  (OpenCV 格式)
```

### 5.3 畸变模型对比

| 模型 | 原厂格式 | OpenCV 格式 |
|------|---------|------------|
| 径向畸变 | k1, k2, k3, k4 (4参数) | k1, k2, k3 (3参数, 多项式) |
| 切向畸变 | 无 (包含在 lens.lua 查表中) | p1, p2 |
| 查表 | lens.lua angle→r 映射 | 无 |

### 5.4 display.c 改造（同方案 A）

从 quad 改成 mesh，代码结构相同。

### 5.5 对 AI 检测框的影响

**当前架构（不改推理）：**
- 推理仍然输入原始鱼眼图像
- 检测框坐标 (x, y, w, h) 在鱼眼图像中
- 显示层使用矫正 mesh 渲染，但检测框绘制需要坐标映射

**坐标映射方案：**
```
方案 B1: 检测框画在原始图上，不矫正
  - 显示: 4路显示矫正画面 + 原始图的检测框 (框位置不对齐)
  
方案 B2: 检测框坐标反投影到矫正图
  - 将鱼眼图中的检测框角点通过畸变模型映射到矫正图
  - 矫正图中重绘检测框
  
方案 B3: 保持 2×2 原始图显示不变，只新增矫正视图窗口
  - 不破坏现有 baseline
```

**推荐**: 先做方案 B3（独立矫正视图），后续再决定是否统一。

---

## 6. 实施建议：分阶段推进

### 阶段 1：只调查，不改代码（当前阶段 ✓）

- [x] 提取原厂标定文件
- [x] 分析矫正模型
- [x] 输出可复用分析

### 阶段 2：离线验证（建议下一步）

- [ ] 用 Python 实现 UV mesh 生成，离线可视化验证
- [ ] 从 calibinfo.lua 解析参数
- [ ] 生成一张矫正后的图像，确认效果

### 阶段 3：最小侵入集成

- [ ] 在 display.c 中新增 mesh shader 通路（不影响现有 quad 通路）
- [ ] 用环境变量或配置开关选择 quad/mesh 模式
- [ ] 4 路独立渲染矫正视图

### 阶段 4：优化和融合

- [ ] 根据效果调 FOV/SingleAngle
- [ ] 决定是否将 AI 检测框映射到矫正图

---

## 7. 明确不要改的内容

| 模块 | 原因 |
|------|------|
| `capture.c` | 视频采集不受影响 |
| `inference.cc` | 推理继续吃原始图 |
| `postprocess.cc` | 后处理不变 |
| RGA 预处理 | 独立通路，不涉及 |
| `pipeline.c` | 主链路不变 |
| `encoder.c` | 编码不变 |
| `frame.h` | 帧结构不变 |
| `start.sh` / `start_display.sh` | baseline 启动脚本不变 |
| 4 路 2×2 显示 baseline | 作为回退模式保留 |

---

## 8. 总结

1. **原厂使用 GPU mesh/shader 做矫正**，不是 CPU remap 或 RGA
2. **4 路摄像头各有独立内外参**，存储在 `/userdata/avm/cali/calibinfo.lua`
3. **Mesh 是运行时生成的**，通过 C++ 函数根据 `mesh_param.lua` 参数生成
4. **lens.lua 包含 42 个镜头畸变查表**，"6028" 是当前使用的型号
5. **可复用参数**：内参 (Center, Focal, DistorParam)、外参 (CameraPos, LookatPos)、畸变查表
6. **不能直接复用**：C++ .so 库（闭源，Lua 绑定架构）
7. **需要自己实现**：UV mesh 生成算法 + GL mesh 渲染管线
8. **工作量**: 方案 A 约 2-3 天，方案 B 约 3-5 天（含标定）
