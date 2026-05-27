# AVM Layout Display Mode — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add AVM_MODE=1 display layout mimicking automotive 360 surround-view screen — left sidebar + center vehicle placeholder + 4 corrected camera views around it.

**Architecture:** New `display_mode_t` enum replaces `bool fisheye_mode`. disp_open() determines mode from env vars (AVM_MODE > FISHEYE_MODE > grid). disp_draw() dispatches to draw_grid_mode / draw_fisheye_grid_mode / draw_avm_mode. AVM mode renders: black sidebar (flat-color shader), 4 fisheye mesh tiles around a central vehicle placeholder (flat-color shader + mesh shader). All existing code paths preserved.

**Tech Stack:** C + OpenGL ES 2.0 + existing NV12 YUV shader + existing fisheye mesh VBOs. No new dependencies.

---

### Task 1: Add display mode enum and AVM mesh infrastructure

**Files:**
- Modify: `src/display.c`

- [ ] **Step 1: Add display mode enum and AVM fields to struct**

Replace `bool fisheye_mode` with an enum and add AVM-specific mesh and shader fields.

```c
/* Add after #include block at top of display.c: */
typedef enum {
    DISPLAY_MODE_GRID,          /* original 2x2 quad */
    DISPLAY_MODE_FISHEYE_GRID,   /* 2x2 fisheye mesh per tile */
    DISPLAY_MODE_AVM,            /* automotive surround-view layout */
} display_mode_t;

/* In struct display_s, replace:
 *     bool fisheye_mode;
 * with:
 */
    display_mode_t          mode;

/* Add AVM-specific fields after mesh_nidx[4]: */
    /* AVM layout */
    GLuint      avm_sidebar_prog;   /* flat-color shader for sidebar/vehicle */
    GLint       avm_sidebar_pos;    /* a_pos location */
    GLint       avm_sidebar_color;  /* u_color location */
    GLuint      avm_mesh_vpos[4];   /* per-camera position VBO (AVM layout) */
    GLuint      avm_mesh_vtex[4];   /* per-camera texcoord VBO (AVM layout) */
    GLuint      avm_mesh_ibo[4];    /* per-camera index buffer (AVM layout) */
    int         avm_mesh_nidx[4];   /* per-camera index count */
```

- [ ] **Step 2: Commit**

```bash
git add src/display.c
git commit -m "feat: add display_mode_t enum and AVM mesh fields to display struct"
```

---

### Task 2: Add flat-color GLES helper for sidebar and vehicle drawing

**Files:**
- Modify: `src/display.c`

- [ ] **Step 1: Add draw_rect and draw_text helpers**

These use the existing OSD shader (`osd_prog`) to render colored rectangles. No new shader compilation needed — just reuse `osd_prog` with different `u_color` values.

```c
/* Add after import_nv12() function, before disp_open(): */

/* Draw a filled rectangle in NDC space [-1,1] */
static void draw_filled_rect(display_t *d,
                              float x0, float y0, float x1, float y1,
                              float r, float g, float b, float a)
{
    glUseProgram(d->osd_prog);
    glUniform4f(d->osd_color_loc, r, g, b, a);
    float v[] = { x0,y0, x1,y0, x1,y1, x0,y1 };
    glVertexAttribPointer(d->osd_pos, 2, GL_FLOAT, GL_FALSE, 0, v);
    glEnableVertexAttribArray(d->osd_pos);
    glDrawArrays(GL_TRIANGLE_FAN, 0, 4);
    glDisableVertexAttribArray(d->osd_pos);
}

/* Draw a line loop rectangle (outline only) */
static void draw_outline_rect(display_t *d,
                               float x0, float y0, float x1, float y1,
                               float r, float g, float b, float a)
{
    glUseProgram(d->osd_prog);
    glUniform4f(d->osd_color_loc, r, g, b, a);
    glLineWidth(2.0f);
    float v[] = { x0,y0, x1,y0, x1,y1, x0,y1 };
    glVertexAttribPointer(d->osd_pos, 2, GL_FLOAT, GL_FALSE, 0, v);
    glEnableVertexAttribArray(d->osd_pos);
    glDrawArrays(GL_LINE_LOOP, 0, 4);
    glDisableVertexAttribArray(d->osd_pos);
}

/* Draw a vehicle placeholder at center (x_center, y_center) in NDC.
 * Draws a simplified car shape: dark body rectangle + lighter roof rectangle. */
static void draw_vehicle_placeholder(display_t *d, float cx, float cy,
                                      float body_w, float body_h)
{
    float bw2 = body_w * 0.5f, bh2 = body_h * 0.5f;

    /* Car body (dark gray rounded-ish via 2 overlapping rects) */
    draw_filled_rect(d, cx - bw2,       cy - bh2,
                        cx + bw2,       cy + bh2,
                        0.15f, 0.15f, 0.18f, 1.0f);

    /* Roof (lighter gray, smaller) */
    float roof_w = body_w * 0.55f, roof_h = body_h * 0.40f;
    float rw2 = roof_w * 0.5f, rh2 = roof_h * 0.5f;
    draw_filled_rect(d, cx - rw2, cy - rh2,
                        cx + rw2, cy + rh2,
                        0.28f, 0.28f, 0.32f, 1.0f);

    /* Front windshield indicator (small lighter bar) */
    draw_filled_rect(d, cx - rw2 * 0.7f, cy + rh2,
                        cx + rw2 * 0.7f, cy + rh2 + 0.015f,
                        0.22f, 0.50f, 0.70f, 1.0f);
}
```

- [ ] **Step 2: Commit**

```bash
git add src/display.c
git commit -m "feat: add GLES draw helpers — filled rect, outline, vehicle placeholder"
```

---

### Task 3: Add AVM sidebar drawing

**Files:**
- Modify: `src/display.c`

- [ ] **Step 1: Add draw_sidebar function**

```c
/* Draw left sidebar with safety text and button placeholders.
 * x0..x1: sidebar NDC x-range.  Text is simulated via colored bars. */
static void draw_sidebar(display_t *d, float x0, float x1)
{
    float y_top    =  0.95f;
    float y_bottom = -0.95f;
    float margin   =  0.02f;

    /* Sidebar background */
    draw_filled_rect(d, x0, y_bottom, x1, y_top,
                     0.05f, 0.05f, 0.06f, 1.0f);

    /* Safety text placeholder — white bar with small colored markers */
    float text_y0 = 0.88f, text_y1 = 0.92f;
    draw_filled_rect(d, x0 + margin, text_y0, x1 - margin, text_y1,
                     0.85f, 0.85f, 0.85f, 1.0f);   /* light "text" bar */

    /* Button placeholders — 4 stacked rounded-rect outlines */
    float btn_h = 0.06f, btn_gap = 0.02f;
    float btn_y = 0.75f;
    const char *btn_labels[] = {"前视", "后视", "左视", "右视"};
    for (int i = 0; i < 4; i++) {
        float by0 = btn_y - btn_h, by1 = btn_y;
        draw_filled_rect(d, x0 + margin, by0, x1 - margin, by1,
                         0.12f, 0.12f, 0.13f, 1.0f);  /* button bg */
        draw_outline_rect(d, x0 + margin, by0, x1 - margin, by1,
                          0.25f, 0.25f, 0.27f, 1.0f); /* button border */
        /* Tiny indicator dot */
        draw_filled_rect(d, x0 + margin + 0.005f, by0 + 0.01f,
                            x0 + margin + 0.015f, by1 - 0.01f,
                            0.2f, 0.6f, 0.2f, 1.0f);
        btn_y = by0 - btn_gap;
    }

    /* Bottom indicator dots */
    float dot_y = -0.70f;
    for (int i = 0; i < 4; i++) {
        draw_filled_rect(d, x0 + 0.03f, dot_y, x0 + 0.05f, dot_y + 0.03f,
                         0.3f, 0.3f, 0.6f, 1.0f);
        dot_y -= 0.05f;
    }

    /* Separator line between sidebar and main area */
    draw_filled_rect(d, x1 - 0.002f, y_bottom, x1 + 0.002f, y_top,
                     0.18f, 0.18f, 0.20f, 1.0f);
}
```

- [ ] **Step 2: Commit**

```bash
git add src/display.c
git commit -m "feat: add AVM sidebar — dark panel, text placeholder, button outlines"
```

---

### Task 4: Update disp_open() for new mode dispatch

**Files:**
- Modify: `src/display.c`

- [ ] **Step 1: Replace mode detection logic**

Replace the current `FISHEYE_MODE` detection block with a unified mode selector, and regenerate AVM-layout meshes when `AVM_MODE=1`.

```c
    /* ---- Display mode selection ---- */
    {
        const char *avm = getenv("AVM_MODE");
        const char *fm  = getenv("FISHEYE_MODE");
        if (avm && avm[0] == '1')
            d->mode = DISPLAY_MODE_AVM;
        else if (fm && fm[0] == '1')
            d->mode = DISPLAY_MODE_FISHEYE_GRID;
        else
            d->mode = DISPLAY_MODE_GRID;
    }

    if (d->mode == DISPLAY_MODE_FISHEYE_GRID || d->mode == DISPLAY_MODE_AVM) {
        /* Compile mesh shader once, shared by fisheye-grid and AVM modes */
        d->mesh_prog = glCreateProgram();
        { GLuint v = compile_shader(GL_VERTEX_SHADER, vert_src);
          GLuint f = compile_shader(GL_FRAGMENT_SHADER, frag_src);
          glAttachShader(d->mesh_prog, v);
          glAttachShader(d->mesh_prog, f);
          glLinkProgram(d->mesh_prog);
          glDeleteShader(v); glDeleteShader(f); }
    }

    if (d->mode == DISPLAY_MODE_FISHEYE_GRID) {
        const char *ff = getenv("FISHEYE_FOV");
        float fov_per_cam[4] = { 130.0f, 170.0f, 170.0f, 170.0f };
        if (ff) {
            char buf[64]; strncpy(buf, ff, 63); buf[63] = 0;
            char *tok = strtok(buf, ",");
            for (int j = 0; j < 4 && tok; j++, tok = strtok(NULL, ","))
                fov_per_cam[j] = atof(tok);
        }
        printf("[display] fisheye mesh FOV: cam0=%.0f cam1=%.0f cam2=%.0f cam3=%.0f\n",
               fov_per_cam[0], fov_per_cam[1], fov_per_cam[2], fov_per_cam[3]);

        int cols = (d->n_cams <= 2) ? d->n_cams : 2;
        int rows = (d->n_cams <= 2) ? 1 : 2;
        float qw = 2.0f / cols, qh = 2.0f / rows;
        for (int i = 0; i < d->n_cams; i++) {
            int col = i % cols, row = i / cols;
            float x0 = -1.0f + col * qw;
            float y0 =  1.0f - (row + 1) * qh;
            fisheye_mesh_t m;
            if (fisheye_mesh_build(&m, &g_fisheye_cams[i],
                                   x0, y0, qw, qh,
                                   960, 540, fov_per_cam[i]) == 0) {
                d->mesh_vbo_pos[i] = m.vbo_pos;
                d->mesh_vbo_tex[i] = m.vbo_tex;
                d->mesh_ibo[i]     = m.ibo;
                d->mesh_nidx[i]    = m.num_indices;
            }
        }
        printf("[display] fisheye mesh mode ON (%d cameras)\n", d->n_cams);
    }

    if (d->mode == DISPLAY_MODE_AVM) {
        /* AVM layout: sidebar 12% width, main area 88% width for 4 cameras */
        const float sidebar_w = 0.24f;   /* NDC: 12% of screen width */
        const float main_x0   = -1.0f + sidebar_w;
        const float main_x1   =  1.0f;
        const float main_w    = main_x1 - main_x0;
        const float main_h    = 2.0f;
        const float main_cx   = (main_x0 + main_x1) * 0.5f;
        const float main_cy   = 0.0f;

        /* Vehicle placeholder area in center */
        const float veh_w = main_w * 0.15f;
        const float veh_h = main_h * 0.25f;

        /* AVM tile layout: 4 fisheye views around the vehicle.
         * cam0=front(top), cam1=right(right), cam2=rear(bottom), cam3=left(left) */
        float fov_avm[4] = { 120.0f, 170.0f, 170.0f, 170.0f };
        struct { float x0, y0, x1, y1; } avm_tiles[4] = {
            /* Front */  { main_cx - veh_w,  main_cy + veh_h * 1.8f, main_cx + veh_w,  main_cy + veh_h * 1.8f + main_h * 0.35f },
            /* Right */  { main_cx + veh_w * 1.2f, main_cy - veh_h * 0.6f, main_cx + veh_w * 1.2f + main_w * 0.25f, main_cy + veh_h * 0.6f },
            /* Rear  */  { main_cx - veh_w,  main_cy - veh_h * 1.8f - main_h * 0.35f, main_cx + veh_w,  main_cy - veh_h * 1.8f },
            /* Left  */  { main_cx - veh_w * 1.2f - main_w * 0.25f, main_cy - veh_h * 0.6f, main_cx - veh_w * 1.2f, main_cy + veh_h * 0.6f },
        };

        for (int i = 0; i < d->n_cams; i++) {
            float tx0 = avm_tiles[i].x0, ty0 = avm_tiles[i].y0;
            float tw  = avm_tiles[i].x1 - tx0;
            float th  = avm_tiles[i].y1 - ty0;
            fisheye_mesh_t m;
            if (fisheye_mesh_build(&m, &g_fisheye_cams[i],
                                   tx0, ty0, tw, th,
                                   480, 360, fov_avm[i]) == 0) {
                d->avm_mesh_vpos[i] = m.vbo_pos;
                d->avm_mesh_vtex[i] = m.vbo_tex;
                d->avm_mesh_ibo[i]  = m.ibo;
                d->avm_mesh_nidx[i] = m.num_indices;
            }
        }
        printf("[display] AVM mode ON (%d cameras)\n", d->n_cams);
    }
```

- [ ] **Step 2: Commit**

```bash
git add src/display.c
git commit -m "feat: add display mode dispatch — AVM_MODE, FISHEYE_MODE, grid baseline"
```

---

### Task 5: Update disp_draw() for AVM rendering path

**Files:**
- Modify: `src/display.c`

- [ ] **Step 1: Replace disp_draw() body with mode dispatch**

Replace the current disp_draw() implementation with:

```c
void disp_draw(display_t *d)
{
    if (!d) return;

    glClear(GL_COLOR_BUFFER_BIT);
    glViewport(0, 0, d->w, d->h);

    switch (d->mode) {
    case DISPLAY_MODE_GRID:
        draw_grid_mode(d);
        break;
    case DISPLAY_MODE_FISHEYE_GRID:
        draw_fisheye_grid_mode(d);
        break;
    case DISPLAY_MODE_AVM:
        draw_avm_mode(d);
        break;
    }

    /* Detection overlay (shared across all modes) */
    draw_detection_overlay(d);

    eglSwapBuffers(d->egl_dpy, d->egl_surf);
}
```

- [ ] **Step 2: Add draw_grid_mode() — existing quad logic extracted**

```c
/* Original 2x2 quad rendering (extracted from disp_draw) */
static void draw_grid_mode(display_t *d)
{
    glUseProgram(d->program);

    int cols = (d->n_cams <= 2) ? d->n_cams : 2;
    int rows = (d->n_cams <= 2) ? 1 : 2;
    float qw = 2.0f / cols, qh = 2.0f / rows;

    GLint u_texY  = glGetUniformLocation(d->program, "u_texY");
    GLint u_texUV = glGetUniformLocation(d->program, "u_texUV");

    for (int i = 0; i < d->n_cams; i++) {
        if (!d->has_frame[i]) continue;
        int col = i % cols, row = i / cols;
        float x0 = -1.0f + col * qw, x1 = x0 + qw;
        float y1 =  1.0f - row * qh, y0 = y1 - qh;

        glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, d->texY[i]);
        glActiveTexture(GL_TEXTURE1); glBindTexture(GL_TEXTURE_2D, d->texUV[i]);
        glUniform1i(u_texY, 0); glUniform1i(u_texUV, 1);

        float verts[] = { x0,y0,0,0, x1,y0,1,0, x1,y1,1,1, x0,y1,0,1 };
        glVertexAttribPointer(d->loc_pos, 2, GL_FLOAT, GL_FALSE, 16, verts);
        glVertexAttribPointer(d->loc_tex, 2, GL_FLOAT, GL_FALSE, 16, verts+2);
        glEnableVertexAttribArray(d->loc_pos);
        glEnableVertexAttribArray(d->loc_tex);
        glDrawArrays(GL_TRIANGLE_FAN, 0, 4);
        glDisableVertexAttribArray(d->loc_pos);
        glDisableVertexAttribArray(d->loc_tex);
    }
}
```

- [ ] **Step 3: Add draw_fisheye_grid_mode() — existing mesh logic extracted**

```c
/* 2x2 fisheye mesh rendering (extracted from disp_draw) */
static void draw_fisheye_grid_mode(display_t *d)
{
    glUseProgram(d->mesh_prog);
    GLint u_texY  = glGetUniformLocation(d->mesh_prog, "u_texY");
    GLint u_texUV = glGetUniformLocation(d->mesh_prog, "u_texUV");
    GLint loc_pos = glGetAttribLocation(d->mesh_prog, "a_pos");
    GLint loc_tex = glGetAttribLocation(d->mesh_prog, "a_tex");

    for (int i = 0; i < d->n_cams; i++) {
        if (!d->has_frame[i]) continue;
        glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, d->texY[i]);
        glActiveTexture(GL_TEXTURE1); glBindTexture(GL_TEXTURE_2D, d->texUV[i]);
        glUniform1i(u_texY, 0); glUniform1i(u_texUV, 1);

        glBindBuffer(GL_ARRAY_BUFFER, d->mesh_vbo_pos[i]);
        glVertexAttribPointer(loc_pos, 2, GL_FLOAT, GL_FALSE, 0, 0);
        glEnableVertexAttribArray(loc_pos);
        glBindBuffer(GL_ARRAY_BUFFER, d->mesh_vbo_tex[i]);
        glVertexAttribPointer(loc_tex, 2, GL_FLOAT, GL_FALSE, 0, 0);
        glEnableVertexAttribArray(loc_tex);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, d->mesh_ibo[i]);
        glDrawElements(GL_TRIANGLES, d->mesh_nidx[i], GL_UNSIGNED_SHORT, 0);
        glDisableVertexAttribArray(loc_pos);
        glDisableVertexAttribArray(loc_tex);
    }
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
}
```

- [ ] **Step 4: Add draw_avm_mode() — AVM layout rendering**

```c
/* AVM surround-view layout: sidebar + 4 corrected views + vehicle placeholder */
static void draw_avm_mode(display_t *d)
{
    /* 1. Black background fill */
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    /* 2. Sidebar (left ~12% of screen) */
    const float sb_x0 = -1.0f;
    const float sb_x1 = -1.0f + 0.24f;
    draw_sidebar(d, sb_x0, sb_x1);

    /* 3. Render 4 fisheye-corrected camera views */
    glUseProgram(d->mesh_prog);
    GLint u_texY  = glGetUniformLocation(d->mesh_prog, "u_texY");
    GLint u_texUV = glGetUniformLocation(d->mesh_prog, "u_texUV");
    GLint loc_pos = glGetAttribLocation(d->mesh_prog, "a_pos");
    GLint loc_tex = glGetAttribLocation(d->mesh_prog, "a_tex");

    for (int i = 0; i < d->n_cams; i++) {
        if (!d->has_frame[i]) continue;
        glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, d->texY[i]);
        glActiveTexture(GL_TEXTURE1); glBindTexture(GL_TEXTURE_2D, d->texUV[i]);
        glUniform1i(u_texY, 0); glUniform1i(u_texUV, 1);

        glBindBuffer(GL_ARRAY_BUFFER, d->avm_mesh_vpos[i]);
        glVertexAttribPointer(loc_pos, 2, GL_FLOAT, GL_FALSE, 0, 0);
        glEnableVertexAttribArray(loc_pos);
        glBindBuffer(GL_ARRAY_BUFFER, d->avm_mesh_vtex[i]);
        glVertexAttribPointer(loc_tex, 2, GL_FLOAT, GL_FALSE, 0, 0);
        glEnableVertexAttribArray(loc_tex);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, d->avm_mesh_ibo[i]);
        glDrawElements(GL_TRIANGLES, d->avm_mesh_nidx[i], GL_UNSIGNED_SHORT, 0);
        glDisableVertexAttribArray(loc_pos);
        glDisableVertexAttribArray(loc_tex);
    }
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);

    /* 4. Vehicle placeholder at center of main area */
    const float main_cx = (sb_x1 + 1.0f) * 0.5f;
    const float main_cy = 0.0f;
    draw_vehicle_placeholder(d, main_cx, main_cy, 0.12f, 0.22f);
}
```

- [ ] **Step 5: Extract detection overlay into separate function**

```c
/* Detection overlay — shared by all display modes */
static void draw_detection_overlay(display_t *d)
{
    pthread_mutex_lock(&d->det_lock);

    int cols = 2, rows = 2;
    float qw = 2.0f / cols, qh = 2.0f / rows;
    static const float ch_colors[4][4] = {
        {1,0,0,1}, {0,1,0,1}, {0,0,1,1}, {1,1,0,1},
    };

    glUseProgram(d->osd_prog);
    glLineWidth(3.0f);

    for (int cam = 0; cam < d->n_cams && cam < 4; cam++) {
        if (!d->has_frame[cam]) continue;
        if (d->det_count[cam] <= 0) continue;

        int col = cam % cols, row = cam / cols;
        float tx0 = -1.0f + col * qw;
        float ty1 =  1.0f - row * qh;
        float xs = qw / 1920.0f;
        float ys = qh / 1080.0f;

        glUniform4fv(d->osd_color_loc, 1, ch_colors[cam]);

        for (int i = 0; i < d->det_count[cam]; i++) {
            detection_t *dt = &d->dets[cam][i];
            if (dt->class_id != 0) continue;
            float bx = tx0 + dt->x * xs;
            float by = ty1 - (dt->y + dt->h) * ys;
            float bw = dt->w * xs, bh = dt->h * ys;
            float v[] = { bx,by, bx+bw,by, bx+bw,by+bh, bx,by+bh };
            glVertexAttribPointer(d->osd_pos, 2, GL_FLOAT, GL_FALSE, 0, v);
            glEnableVertexAttribArray(d->osd_pos);
            glDrawArrays(GL_LINE_LOOP, 0, 4);
            glDisableVertexAttribArray(d->osd_pos);
        }
    }
    pthread_mutex_unlock(&d->det_lock);
}
```

- [ ] **Step 6: Commit**

```bash
git add src/display.c
git commit -m "feat: add AVM draw mode — sidebar, vehicle placeholder, 4 corrected views"
```

---

### Task 6: Update disp_close() for AVM cleanup

**Files:**
- Modify: `src/display.c`

- [ ] **Step 1: Add AVM VBO cleanup in disp_close()**

```c
    /* In disp_close(), replace the fisheye_mode cleanup with mode-based: */
    if (d->mode == DISPLAY_MODE_FISHEYE_GRID || d->mode == DISPLAY_MODE_AVM) {
        glDeleteProgram(d->mesh_prog);
    }
    for (int i = 0; i < d->n_cams; i++) {
        if (d->mesh_vbo_pos[i]) glDeleteBuffers(1, &d->mesh_vbo_pos[i]);
        if (d->mesh_vbo_tex[i]) glDeleteBuffers(1, &d->mesh_vbo_tex[i]);
        if (d->mesh_ibo[i])     glDeleteBuffers(1, &d->mesh_ibo[i]);
        if (d->avm_mesh_vpos[i]) glDeleteBuffers(1, &d->avm_mesh_vpos[i]);
        if (d->avm_mesh_vtex[i]) glDeleteBuffers(1, &d->avm_mesh_vtex[i]);
        if (d->avm_mesh_ibo[i])  glDeleteBuffers(1, &d->avm_mesh_ibo[i]);
    }
```

- [ ] **Step 2: Commit**

```bash
git add src/display.c
git commit -m "fix: add AVM VBO cleanup in disp_close"
```

---

### Task 7: Build, deploy, and verify all 3 modes

**Files:**
- Deploy: `rk3568_camera`

- [ ] **Step 1: Build**

```bash
cd /home/rrn/rk3568-camera && make 2>&1 | tail -5
```
Expected: `=== Build OK: rk3568_camera ===`

- [ ] **Step 2: Push**

```bash
adb push rk3568_camera /userdata/rk3568_camera && adb shell chmod +x /userdata/rk3568_camera
```

- [ ] **Step 3: Test baseline**

```bash
adb shell "cd /userdata && LD_LIBRARY_PATH=/usr/lib timeout 4 ./rk3568_camera -c 1 --no-enc --no-disp 2>&1" | head -5
```
Expected: Normal capture, no errors.

- [ ] **Step 4: Test fisheye grid mode (unchanged)**

```bash
adb shell "cd /userdata && LD_LIBRARY_PATH=/usr/lib timeout 4 ./rk3568_camera -c 1 --no-disp --no-enc 2>&1" | head -5
```
Expected: FISHEYE_MODE=1 still works as before.

- [ ] **Step 5: Test AVM mode (from board console)**

```bash
cd /userdata && AVM_MODE=1 LD_LIBRARY_PATH=/usr/lib ./rk3568_camera -c 4 --no-enc
```
Expected:
- Left black sidebar with button placeholders
- Center vehicle placeholder
- 4 corrected camera views around vehicle
- display fps normal

- [ ] **Step 6: Commit any fixes**

```bash
git add src/display.c && git commit -m "fix: AVM mode deployment fixes"
```

---

## File modification summary

| File | Change |
|------|--------|
| `src/display.c` | Add mode enum, helpers, sidebar, vehicle, AVM layout, mode dispatch, cleanup |
| All other files | UNCHANGED |

## Mode priority

1. `AVM_MODE=1` → DISPLAY_MODE_AVM (highest)
2. `FISHEYE_MODE=1` → DISPLAY_MODE_FISHEYE_GRID
3. (default) → DISPLAY_MODE_GRID

## Test commands

```bash
# Grid baseline
cd /userdata && ./rk3568_camera --no-enc

# Fisheye grid (unchanged)
FISHEYE_MODE=1 ./rk3568_camera --no-enc

# AVM layout (new)
AVM_MODE=1 ./rk3568_camera --no-enc

# AVM with custom FOV
AVM_MODE=1 FISHEYE_FOV=120,170,170,170 ./rk3568_camera --no-enc
```
