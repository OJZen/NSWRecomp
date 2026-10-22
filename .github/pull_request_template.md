说明具体问题、修复后的行为及影响范围。

- 验证：命令、实际结果、对应候选版本。
- 未验证或已知限制：区分分析、生成、编译、隔离执行与游戏实玩。
- 来源变化：涉及派生源码时说明许可证、来源版本、SOURCE.json 和哈希更新。

提交前按 [CONTRIBUTING.md](https://github.com/OJZen/NSWRecomp/blob/main/CONTRIBUTING.md) 检查：

- [ ] 只包含本任务授权范围；逐项检查文件与全部传入提交。
- [ ] 相关回归及 `git diff --check` 通过。
- [ ] `python3 tools/check_publication.py --ref HEAD` 通过；没有私有输入或派生产物。
- [ ] 未合入首次公开整理前的私人历史、未验收实验或其他 agent 的改动。
- [ ] 文档中的证明范围与实际验证一致；合并及推送已有明确授权。
