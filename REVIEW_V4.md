# REVIEW_V4.md — TieredVol v1.1.0 修复确认报告

**日期:** 2026-07-16
**版本:** 1.1.0
**编译器:** gcc -Wall -Wextra -Wpedantic -std=gnu11 -O2（零警告）

---

## 已修复 Bug 列表

| # | 严重度 | Bug 描述 | 修复文件 | 验证方式 |
|---|--------|----------|----------|----------|
| 1 | HIGH | `parse_bench_output()` 并行模式匹配错误行 | tiered_ui.c | 单元测试 + CLI |
| 2 | HIGH | `bench_disk_done()` 并行模式永远返回 0 | tiered_ui.c | 单元测试 |
| 3 | HIGH | `screen_status()` lvs 命令字符串拼接错误 | tiered_ui.c | 单元测试 + CLI |
| 4 | HIGH | TUI 输入未验证直接传 CLI | tiered_ui.c | 单元测试 + CLI |
| 5 | MEDIUM | `parse_disk_list()` 不解析 T 后缀 | tiered_ui.c | 单元测试 |
| 6 | HIGH | CLI `--fs` 无白名单（可执行任意命令） | tiered_setup.c | CLI 集成测试 |
| 7 | CRITICAL | `parse_bench_output` 搜 `Write:` 但并行输出是 `Write` 无冒号 | tiered_ui.c | 单元测试 + CLI |

---

## 测试结果

### 单元测试 (tests/test_tui.c)

```
=== Results: 22/22 passed ===
```

- parse_bench_output 并行 2 disk: PASS
- parse_bench_output 并行 3 disk 乱序: PASS
- parse_bench_output Started 行不匹配: PASS
- parse_bench_output disk 不存在: PASS
- bench_disk_done 并行输出: PASS
- bench_disk_done 旧格式拒绝: PASS
- bench_disk_done 空缓冲区: PASS
- lvs 命令构造: PASS

### CLI 集成测试

| 测试项 | 结果 |
|--------|------|
| `--version` | PASS — TieredVol 1.1.0 |
| `--help` | PASS — 显示完整用法 |
| `--list` | PASS — 4 disk（sda ROOT、sdb 465G、sdc 232G、nvme0n1 931G） |
| `--bench --disks sdb,sdc`（并行 2 disk） | PASS — sdb 340/395, sdc 393/442 MB/s |
| `--bench --disks sdb,sdc,nvme0n1`（并行 3 disk） | PASS — nvme0n1 1025/1117, sdb 278/386, sdc 213/354 MB/s |
| `--bench --disks sdb --sequential`（顺序） | PASS — sdb 401/384 MB/s |
| `--bench --disks 'sdb;rm -rf /'` | PASS — 拒绝 |
| `--create --name 'a;b'` | PASS — 拒绝 |
| `--create --fs 'ext4;rm -rf /'` | PASS — 拒绝 |
| `--create --fs ntfs` | PASS — 拒绝 |
| `--create --mount test`（无 `/`） | PASS — 拒绝 |
| `--create --name final --disks sdb:100,sdc:50` | PASS — 150GB, 2 stripes |
| `df -h /mnt/final` | PASS — 98G |
| `--status` | PASS — 显示 3 DM targets + config |
| `--destroy --name final` | PASS — 完整清理 |

---

## 安全确认

- [x] 名称注入 `sdb;rm -rf /` 被 CLI 和 bench 同时拒绝
- [x] 文件系统注入 `ext4;rm -rf /` 被 CLI 白名单拒绝
- [x] 非法文件系统 `ntfs` 被拒绝
- [x] 挂载点无 `/` 前缀被拒绝
- [x] 所有用户输入经 `is_valid_name()` 白名单验证
- [x] 建立失败自动回滚（umount → lvremove → vgremove → pvremove → dmsetup remove）
