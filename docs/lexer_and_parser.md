# 词法分析与语法分析（设计文档）

本文档说明 `feipiaocc` 的前端整体分层：从源文件到 `PPToken`、再到 `Token`、再到 AST（语法树）。目标是让实现过程“可分阶段推进、可测试、可定位”，并且避免把职责混在一起导致后续难维护。

参考资料：

- `feipiaocc/docs/C11std.pdf`（C11 标准附录 A：语法摘要）
- `feipiaocc/docs/lex.md`（你整理的 FIRST 集合/产生式摘要）
- `feipiaocc/docs/pp_token_origin.md`（宏展开可定位：来源链）

---

## 0. 总览：流水线与数据结构

一个翻译单元（`.c` 文件）的推荐流水线：

1) **Preprocessing-token tokenizer**
   - 输入：`SourceFile`（文件路径 + 内容缓冲）
   - 输出：`PPToken` 流（preprocessing-token + NEWLINE/EOF）

2) **Preprocess（预处理）**
   - 输入：`PPToken` 流
   - 输出：`PPToken` 流（执行指令 + 宏展开 + include 展开后的平铺结果）
   - 备注：宏展开发生在此阶段，来源链也在此阶段建立

3) **Lexer（编译 token 化）**
   - 输入：预处理后的 `PPToken` 流
   - 输出：`Token` 流（keyword/identifier/constant/string-literal/punctuator…）

4) **Parser（语法分析）**
   - 输入：`Token` 流
   - 输出：AST（语法树）+（后续）类型信息

关键点：

- `PPToken` 是“预处理域”的 token（C11 preprocessing-token）。
- `Token` 是“编译域”的 token（C11 token：keyword/identifier/constant/string-literal/punctuator）。
- 宏与 `#include/#if...` 只能在预处理域完成；语法分析只看最终 `Token`。

---

## 1. SourceFile（输入模型）

本文档把“源文件”抽象为 `SourceFile`（未必是最终 C 类型名）：

- `path`：用于诊断输出
- `contents`：NUL 结尾缓冲区

规范化（建议放在“打开文件”处统一做）：

- 文件末尾确保有 `\n`
- 归一化 CRLF 为 LF
- 拼接反斜杠续行：`\` + `\n` 删除（translation phase 2）

这样 tokenizer 的行为可预测，诊断也更稳定。

---

## 2. Preprocessing-token tokenizer（字符 → PPToken）

### 2.1 输入/输出

输入：

- `SourceFile.contents`（已规范化）

输出：

- `PPToken` 流，token kind 至少包含：
  - `IDENTIFIER`
  - `PP_NUMBER`
  - `CHARACTER_CONSTANT`
  - `STRING_LITERAL`
  - `PUNCTUATOR`（maximal munch）
  - `OTHER`（“非空白且不属于以上”的单字符）
  - `NEWLINE`（为了指令解析）
  - `EOF`

说明：`header-name` 不在 tokenizer 阶段直接产出（由 preprocess 的 `#include` 解析阶段组合/解释）。

### 2.2 规则（不回溯，最大吞噬）

分发入口使用 FIRST 集合思想（见 `lex.md`）：

- `"`/`'`/`u8"`/`u"`/`U"`/`L"` → 扫描字符串/字符常量
- digit 或 `.` 后接 digit → 扫描 `pp-number`
- nondigit 或 `\u`/`\U` → 扫描 identifier（含 UCN）
- punctuator_first → 扫描 punctuator（maximal munch）
- 其他非空白字符 → `OTHER`

空白与注释：

- 空白（不含换行）跳过，但设置 `has_space=true`
- `//` 注释跳过直到换行
- `/* */` 注释跳过（允许跨行），但换行需要产出 `NEWLINE` token（便于指令按行处理）

### 2.3 位置与诊断（宏展开前的基础）

tokenizer 产出的 `PPToken` 必须携带“拼写位置”（spelling loc）：

- `path`
- `line:col`（建议精确列号）
-（可选）`byte_offset`

宏展开来源链见 `pp_token_origin.md`，在 preprocess 阶段建立。

---

## 3. Preprocess（PPToken → PPToken）

Preprocess 的职责：

1) 解析并执行预处理指令（`#...`）
2) 宏表管理（define/undef）
3) 条件编译（if/ifdef/ifndef/elif/else/endif）
4) `#include` 展开（递归/栈式）
5) 宏展开（对象宏/函数宏、`#`、`##`、hideset/禁止递归展开）

输出仍然是 `PPToken` 流（NEWLINE 是否保留由后续阶段需求决定；通常保留到指令处理结束再可选丢弃）。

### 3.1 `#include` 的展开逻辑（推荐）

在 preprocess 解析到 `#include` 后：

- 读取 include 参数：
  - 若是 `STRING_LITERAL`（如 `"a.h"`），直接取内容作为候选路径
  - 若是 `<` ... `>` 形式，收集 token 直到 `>` 并拼成 header-name 文本
  -（未来）若是 identifier/宏：先宏展开成上述两种形式再处理
- 搜索路径（用户 `-I` + 系统 include + 默认）
- 打开文件得到 `SourceFile`
- 对该文件执行：
  - `tokenize(SourceFile) -> PPToken*`
  - `preprocess(PPToken*) -> PPToken*`
- 将结果 token 流拼接进当前输出流

建议使用 include 栈（而非无限递归），并设置 include 深度上限防止爆栈/循环 include。

### 3.2 宏展开与来源链

宏展开在此阶段执行。每当宏调用产生新 token，必须根据 `pp_token_origin.md` 挂来源链：

- `spelling`：来自宏体或实参
- `origin`：`expanded_at/defined_at` + `parent`（支持宏套宏）

后续 `Token` 与错误打印均依赖这一点。

---

## 4. Lexer（PPToken → Token）

Lexer 的职责是把“预处理域 token”细分成“编译域 token”：

- `IDENTIFIER`：
  - 若命中关键字表 → `TK_KEYWORD`
  - 否则 → `TK_IDENT`
- `PP_NUMBER`：
  - 按 C11 常量规则解析为整数常量/浮点常量（或先保留字符串，延迟到 parser/type 阶段解析）
- `CHARACTER_CONSTANT` / `STRING_LITERAL`：
  - 解析前缀（u8/u/U/L）、转义序列（可先延迟）
- `PUNCTUATOR`：
  - 直接映射为对应 token kind（保留原文本切片）
- `OTHER`：
  - 一般视为 lexer error（“非法字符”），除非你刻意支持实现定义字符

定位信息：

- `Token` 必须携带来源链（从对应 `PPToken` 搬运）以支持宏回溯报错。

---

## 5. Parser（Token → AST）

Parser 的职责：

- 以 C11 语法构建 AST（表达式、语句、声明、类型、函数、翻译单元）
- 处理优先级/结合性、声明符（declarator）等典型难点

### 5.1 语法分析格式（TBD）

你说“语法分析的格式还不清晰”，这里先列出需要你确认的设计点：

1) AST 是否采用 chibicc 类结构（NodeKind + Node* 链接）？
2) 解析函数命名与粒度：按产生式（如 `declarator()`）还是按语义（如 `parse_stmt()`）？
3) 错误恢复策略：直接 `DIE` 还是做同步点恢复（例如遇到 `;`）？
4) typedef-name 识别：在 parser 阶段引入符号表（typdef set）还是延迟？

你确认这些后，我们再把 Parser 部分展开成更具体的接口与文件组织。

---

## 6. 最小可交付阶段划分（建议）

为了避免一次性把宏/预处理/词法/语法全写爆，建议按里程碑推进：

1) preprocessing-token tokenizer + dump（已能自举扫描）
2) preprocess：只支持 `#include` + `#define`（对象宏）+ 简单展开（不做 `##/#`）
3) preprocess：加函数宏 + hideset + 条件编译
4) lexer：关键字/标点分类 + 最小常量解析
5) parser：表达式 + 语句 + 函数定义

