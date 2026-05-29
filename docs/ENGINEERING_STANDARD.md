# WT99P4C5-S1 工程改动标准

本文档为 [`.cursor/rules/engineering-standards.mdc`](../.cursor/rules/engineering-standards.mdc) 的完整说明。

## 1. 目的

- 每次优化改动可追溯、可 Review
- 便于对比 Flash/RAM 前后变化
- 避免重复或冲突修改

## 2. 改动记录文件

所有优化改动写入 **[`OPTIMIZATION_CHANGELOG.md`](OPTIMIZATION_CHANGELOG.md)**，按时间**追加**条目，不覆盖历史记录。

## 3. 单条记录模板

```markdown
## [阶段X.Y] 改动标题 — YYYY-MM-DD

### 改了什么
（一句话摘要）

### 改动位置
| 文件 | 位置 | 变更说明 |
|------|------|----------|
| path/to/file | L123–145 / 函数名 | 具体做了什么 |

### 原因
（为什么要改）

### 预期影响
- Flash：-XXX KB
- PSRAM：-XXX KB
- Internal RAM：-XXX KB
- 性能：...

### 验证
- [ ] idf.py build 通过
- [ ] idf.py size 对比
- [ ] 功能回归：...
```

## 4. 配置类改动

### 4.1 文件分工（必须遵守）

| 文件 | 是否进 Git | 用途 |
|------|------------|------|
| `sdkconfig` | 否（`.gitignore`） | 本机当前生效配置，由 defaults merge 生成 |
| `sdkconfig.defaults` | 是 | 全构建共用的基础默认 |
| `sdkconfig.defaults.release` | 是 | Release 覆盖层（日志、字体裁剪、`-Os` 等） |

根目录 `CMakeLists.txt`：`SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.release"`。

**规则：** 以后改 `sdkconfig` 时，凡需保留的配置 **必须同步** 维护 `sdkconfig.defaults` / `sdkconfig.defaults.release`，不能只改本地 `sdkconfig`。

### 4.2 操作流程

1. 确定改动属于「全局默认」还是「仅 Release」。
2. 修改对应 defaults 文件（可同时改两个；release 覆盖 defaults 同名项）。
3. 本地可删 `sdkconfig` 或 `idf.py fullclean` 后 `idf.py build`，确认 merge 结果。
4. 在 CHANGELOG 记录 `CONFIG_*` **旧值 → 新值** 及修改的 defaults 路径。

本地 `menuconfig` 试验：确认后执行 `idf.py save-defconfig` 或手工抄回 defaults，勿只留 `sdkconfig`。

### 4.3 CHANGELOG 示例

```markdown
| 文件 | CONFIG_* | 旧值 → 新值 |
|------|----------|-------------|
| sdkconfig.defaults | CONFIG_LWIP_TCP_SND_BUF_DEFAULT | 5744 → 32768 |
| sdkconfig.defaults.release | CONFIG_LOG_DEFAULT_LEVEL | 3 (INFO) → 2 (WARN) |
```

## 5. 资源排除/删除

示例：

```markdown
CMake 排除：`music_player/gui_music/assets/*_large.c`（22 个文件，预估 ~1 MB）
删除引用：`video_player/assets/breaking_news.c`（~211 KB，无引用）
```

## 6. 阶段末验证

```bash
idf.py build
idf.py size
idf.py size-components
```

将 `idf.py size` 总 Flash 与 `.rodata` 写入 CHANGELOG「验证记录」节，并与基线对比。

## 7. Git 提交建议

- 一个逻辑改动一条 CHANGELOG 条目
- commit message 引用阶段编号，如：`opt(1.1): exclude unused *_large music assets`
