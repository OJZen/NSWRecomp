# 提交、合并与发布要求

适用于人工和 agent。先读 [项目规则](AGENTS.md)、[当前状态](docs/status.md)及相关专题。
开发、提交、合并、推送分别以用户当前授权为界；完成代码不等于获准发布。

## 分支与提交

- 普通任务从最新 `main` 建立 `codex/<主题>` 分支或独立工作树。只有明确授权主线整理的任务才直接修改 `main`。
- 开始前记录分支、HEAD、`git status --short` 和已有差异；结束前再次核对。不得覆盖其他 agent 或用户的改动。
- 一个提交只解决一个可解释的问题，连同必要调用方、回归检查和文档一起提交。
  使用明确文件列表或逐块暂存；禁止 `git add .`、`git add -A`、`git commit -a`。
- 暂存后检查 `git diff --cached --stat`、`git diff --cached`、`git diff --cached --check`。
  提交标题写具体行为，正文说明原因、实际验证与尚未验证的边界；不要粘贴聊天记录或原始日志。
- 作者及提交者使用 GitHub 用户名和该账户的 noreply 邮箱。不得提交个人邮箱、系统账户名、
  主机名、真实用户/卷路径、私网地址、凭据或带认证信息的 URL；示例用相对路径或 `/path/to/...`。

## 文件和证据

- `local/`、`work/` 仅留本机；包、密钥、提取资源、生成 C、着色器捕获、缓存、截图、存档、二进制和原始日志不得进入 Git。
  `.gitignore` 不会保护已经跟踪的文件，也不会清除旧提交。
- 合成测试不得依赖游戏或密钥。新增非平凡逻辑要有最小回归；文档改动检查链接与源码依据。
  CPU/运行时/图形改动按影响执行相关测试，必要时执行 `make test`；不要为纯文档改动重跑游戏。
- 分开记录源码分析、C 生成、编译/链接、隔离执行、真实画面和实玩。
  写明版本、命令、结果、资源限制与未通过项；本地证据用相对路径引用，不把旧二进制的验收转嫁到新提交。
- 修改派生源码时保留许可证和上游来源版本，更新 `SOURCE.json` 中对应的本地改动与文件哈希。
  不修改或清理外部参考仓库，不覆盖存档和正在使用的产物。

## 合并门槛

1. 核对目标确为最新 `main`，检查全部传入提交及最终树差异，而不只检查最后一笔提交。
2. 确认改动属于授权范围，测试对应实际候选，文档没有扩大证明范围；冲突解决后重新检查受影响部分。
3. 实验结果单独评审。目录迁移、整理或“测试通过”不构成自动合并实验的理由。
4. **首次公开整理前的本地分支与迁移标签保留了私人历史，不可直接 merge 到公开 main。**
   从公开 main 新建分支，只迁入逐项审核过的改动；可用 `cherry-pick --no-commit <已审查提交>`，
   检查暂存区后以公开身份重新提交。不要使用 `--allow-unrelated-histories` 或把旧祖先合回主线。
5. 在最终候选执行 `python3 tools/check_publication.py --ref HEAD`。它扫描全部可达历史，拒绝浅克隆并忽略 replace 替代视图；
   一条新提交删除敏感文件不能修复旧历史。发现真实密钥时停止发布，先撤销/轮换再处理历史。

## 本地防误提交与发布

每个新克隆安装仓库 hooks；使用绝对 hooks 路径，让本仓库的旧工作树也使用同一规则：

```sh
git config --local core.hooksPath "$PWD/.githooks"
git config --local user.name YOUR_GITHUB_HANDLE
git config --local user.email YOUR_GITHUB_NOREPLY_EMAIL
git config --local push.default nothing
git config --local push.followTags false
git config --local remote.origin.push refs/heads/main:refs/heads/main
```

`pre-commit` 检查完整暂存区、作者身份与空白错误；`pre-push` 只允许向 `origin` 推送 `main`，
并扫描实际待推送提交及全部祖先。检查只输出位置与类别，不打印匹配到的值。
轻量规则覆盖常见隐私路径、邮箱、凭据、私网地址及不应发布的文件；不能代替人工审查。
第三方许可证中的公开署名、链接与来源哈希必须保留；误报应窄范围审核，不可一概删除署名或绕过检查。

用户明确授权推送且检查完成后，只执行：

```sh
python3 tools/check_publication.py --ref main
git -c push.followTags=false push origin refs/heads/main:refs/heads/main
git ls-remote --heads --tags origin
```

不得使用 `--all`、`--mirror`、`--tags`、强推、`--no-verify` 或临时关闭 hooks 绕过规则。
远程已有不同历史时先获取并核对，不能覆盖。推送后核对远程 main SHA，并确认没有附带其他分支或标签。
Hooks 是本地防误操作措施，不是 GitHub 分支保护；新增远程保护规则需另行配置，不能宣称本文件已经启用服务器强制审批。
