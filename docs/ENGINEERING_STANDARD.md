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

示例：

```markdown
| CONFIG_BT_ENABLED | y → n |
| CONFIG_LOG_DEFAULT_LEVEL | 3 (INFO) → 2 (WARN) |
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
