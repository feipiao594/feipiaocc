# PPToken 来源链设计（宏展开可定位）

目的：预处理阶段（宏展开之后）仍然能够把错误定位到正确的位置，并且在需要时打印宏展开回溯（macro backtrace）。

本设计把“来源链”作为 **预处理 token（PPToken）** 的一等信息：宏展开产生的新 token 必须携带或可查询到其来源链；后续 `PPToken -> Token` 转换时再把这些信息搬运到最终 `Token`。

---

## 1. 背景：为什么必须在 PPToken 阶段做

宏展开发生在预处理阶段，它处理的是 *preprocessing-token* 流：复制宏体 token、把实参 token 插入、进行 `#` 字符串化、`##` 拼接等。这些操作会产生大量“新的 token”。

如果不在 PPToken 阶段保存来源信息，那么当 token 流被展开平铺后，单靠最终 `Token` 的文本切片很难再推回：

- 这个 token 是在什么位置展开出来的（宏调用点）
- 它来自哪个宏（宏名）以及宏定义位置
- 它是否是宏套宏展开的产物（链式回溯）

因此：来源链应在宏展开生成 token 的那一刻建立。

---

## 2. 位置模型：spelling 与 expansion

### 2.1 `spelling location`（拼写位置）

表示 token 的“字面内容”来自哪里。典型情况：

- 源文件 token：spelling 就是该源文件位置。
- 宏体 token：spelling 是宏定义文件中宏体对应 token 的位置。
- 宏实参 token：spelling 是调用点处实参文本对应 token 的位置。

### 2.2 `expansion location`（展开位置）

如果一个 token 是宏展开产物，则它还有一个“展开位置”：宏调用点（例如 `FOO(...)` 的位置）。这用于用户最关心的定位：*你代码里哪里触发了这个宏展开*。

---

## 3. 数据结构（建议）

### 3.1 源码位置 `PPSrcLoc`

需要可打印（path/line/col），也建议保留 byte offset 方便调试与映射。

```c
typedef struct {
  PPFile *file;       // 或 path + file_id
  int byte_offset;    // 从 file->contents 起算
  int line_no;        // 1-based
  int col_no;         // 1-based
} PPSrcLoc;
```

### 3.2 来源链节点 `PPOrigin`

用于宏回溯（宏套宏）。

```c
typedef struct PPOrigin PPOrigin;
struct PPOrigin {
  const char *macro_name; // 建议使用驻留字符串（intern）或指向宏名存储
  PPSrcLoc expanded_at;   // 宏调用点
  PPSrcLoc defined_at;    // 宏定义点（可选但强烈建议）
  PPOrigin *parent;       // 上一层来源（宏套宏/参数来源）
};
```

### 3.3 预处理 token `PPToken`

两种实现都可行：

- **方案 A（推荐）**：token 自带来源链指针（局部性更好，少同步 bug）
- 方案 B：token 仅存 `id`，用 `id -> meta` 表查来源链

本设计文档优先描述方案 A。

```c
typedef struct {
  PPTokenKind kind;
  unsigned id;        // 全局递增（编译单元内即可）
  const char *loc;
  int len;
  bool at_bol;
  bool has_space;

  PPSrcLoc spelling;  // 拼写位置（永远存在）
  PPOrigin *origin;   // 为空表示没有宏展开来源；非空表示可回溯
} PPToken;
```

---

## 4. 生成规则（什么时候填哪些字段）

### 4.1 tokenizer 产出的 PPToken

- `spelling`：来自当前源文件位置（精确到 line/col）
- `origin`：`NULL`

### 4.2 宏展开产出的 PPToken

设宏名为 `FOO`，调用点位置为 `call_loc`，宏定义位置为 `def_loc`。

#### 4.2.1 来自宏体（macro body）的 token

- `spelling`：宏体 token 在宏定义文件中的位置（宏体 token 自己应携带 spelling）
- `origin`：新建一层 `PPOrigin{macro_name="FOO", expanded_at=call_loc, defined_at=def_loc, parent=<调用点 token 的 origin>}`

#### 4.2.2 来自实参替换（macro argument）的 token

实参 token 流可能本身也来自宏展开（已有 origin 链）。

- `spelling`：保持实参 token 的 spelling（它的字面来自调用点附近）
- `origin`：仍然新建一层 `expanded from FOO at call_loc`，并把 `parent` 设为“实参 token 的原 origin”

这样可以同时保留：

1) “这个 token 最终是在 FOO 展开时产生的”
2) “它原本又是从哪来的”（宏套宏、或源文件）

#### 4.2.3 `#` 字符串化 / `##` 拼接产生的新 token

这类 token 通常没有对应“原始拼写 token”。

建议：

- `spelling`：选择一个合理的 token 来源（例如宏体中的 `#` / `##` 所在位置，或调用点位置）
- `origin`：同样挂 `expanded from FOO at call_loc`

---

## 5. 错误定位策略（推荐）

对一个 token `t`：

- 若 `t->origin != NULL`：
  - 主报错位置：`t->origin->expanded_at`（告诉用户“你这里触发了宏”）
  - 追加 note：
    - `expanded from macro 'FOO'`（沿 origin 链回溯）
    - `macro 'FOO' defined here`（若保存 defined_at）
- 若 `t->origin == NULL`：
  - 主报错位置：`t->spelling`

这会让报错信息更贴近用户心智：先指出“你代码哪儿写的/触发的”，再补充“宏内部/定义处”。

---

## 5.1 例子：宏展开后的错误如何定位

### 示例 1：单层宏（来自宏体）

文件 `t.c`：

```c
#define BAD(x) (x + )
int main() { return BAD(1); }
```

预处理展开时，`BAD(1)` 会生成一个新的 token 流，其中 `)` 前面会出现一个不完整的表达式。

对展开后那个“导致语法错误”的 token（例如 `)` 或紧邻的 `+`），我们期望的定位是：

- **主错误位置（expanded_at）**：指向 `t.c` 中 `BAD(1)` 的调用点（告诉用户“你这里触发了宏展开”）。
- **附加 note（defined_at/spelling）**：指出 `BAD` 的定义位置（告诉用户“宏体本身是坏的”）。

也就是：

- `spelling`：来自宏体 `#define BAD(x) (x + )` 中对应 token 的位置
- `origin.expanded_at`：来自 `return BAD(1);` 中 `BAD(1)` 的位置

**错误输出长相（建议）**：

```
error: expected expression
  at t.c:2:21
note: expanded from macro 'BAD'
  at t.c:2:21
note: macro 'BAD' defined here
  at t.c:1:9
```

解释：

- 主错误行列 `t.c:2:21` 选择 **expanded_at**（宏调用点）
- 额外提示把宏名与定义点打印出来，帮助用户快速定位“宏体本身不合法”

### 示例 1.1：对象如何工作（最小模型）

把 `BAD(1)` 展开成 token 流时（伪代码）：

1) 记录调用点 `call_loc = loc_of("BAD(1)")`
2) 对宏体 token 逐个复制生成新 token `t2`
3) 对每个 `t2`：
   - `t2.spelling = body_token.spelling`（来自宏定义处）
   - `t2.origin = new_origin("BAD", expanded_at=call_loc, defined_at=def_loc, parent=NULL)`
4) 后续报错时，拿到任意 `t2` 都能：
   - 用 `t2.origin.expanded_at` 打印主定位（宏调用点）
   - 用 `t2.origin.defined_at` 提示宏定义位置

### 示例 2：嵌套宏（来自实参）

文件 `t.c`：

```c
#define ID(x) x
#define WRAP(x) ID(x)
int main() { return WRAP(1 +); }
```

这里错误的根源在调用点实参 `1 +`，但它是通过 `WRAP -> ID` 两层宏展开才进入最终 token 流的。

对最终导致语法错误的 token（例如行末 `;` 或缺失操作数处附近的 token），建议的定位/回溯为：

- **主错误位置**：指向 `WRAP(1 +)` 的调用点（最外层宏的 expanded_at）。
- **宏回溯**（沿 `origin.parent`）：
  - expanded from `WRAP` at `t.c:<line>:<col>`
  - expanded from `ID` at `t.c:<line>:<col>`（如果你把 `ID(x)` 的调用点也挂链）
- **拼写位置**：`spelling` 指向实参 `1 +` 在源文件中的位置（因为 token 字面来自实参）。

要点：

- 来自“宏体”的 token：`spelling` 指向宏定义处；来自“实参”的 token：`spelling` 指向调用点实参处。
- 无论来自哪里，只要是“因为宏展开才出现”，就应该有 `origin.expanded_at`，并能向外串成链。

**错误输出长相（建议）**：

```
error: expected expression
  at t.c:3:21
note: expanded from macro 'WRAP'
  at t.c:3:21
note: expanded from macro 'ID'
  at t.c:3:16
note: token spelling here
  at t.c:3:24
```

解释：

- 主错误位置仍然选最外层宏调用点（用户最关心“我哪一行触发了宏”）
- 回溯链提供“经过了哪些宏”（从外到内）
- `spelling`（拼写位置）对“来自实参”的 token 很有用：它能指出实参里具体哪一段文本是坏的

### 示例 2.1：对象如何工作（链的拼接规则）

从调用 `WRAP(1 +)` 开始：

- `WRAP(x)` 展开会产生一个 `ID(x)` 的调用点；因此 `ID` 的 `expanded_at` 可以指向 `ID(x)` 那个位置。
- 当 `ID(x)` 把参数 `x` 替换成实参 token `1` `+` 时：
  - 这些 token 的 `spelling` 应保持为“实参在源文件中的位置”
  - 但它们的 `origin` 需要包两层：先记录 `ID` 的展开，再把 `WRAP` 的展开作为 parent（或反过来，取决于你在何处生成 token）

一个可操作的规则是：每进入一层宏展开，就 `origin = new_origin(macro, expanded_at=call_loc, parent=old_origin)`。
这样生成的链总是从“最近一次展开”往外串。

---

## 6. 内存与所有权建议

为了让 `origin` 指针在整个预处理过程中保持有效，推荐：

- `PPOrigin` 节点使用 arena 分配（编译单元生命周期内不释放单个节点）
- `PPToken` 若使用链表流，也建议 arena 分配（减少 malloc/free，且便于复制 token）

宏名字符串建议驻留（intern）或至少保证生命周期覆盖整个编译单元。

---

## 7. 实施步骤（渐进）

1) tokenizer 阶段先做到：
   - `PPToken.id` 全局递增
   - `PPToken.spelling`（path/line/col/offset）
   - `PPToken.origin = NULL`
2) 引入宏表与宏体 token（宏体 token 也应携带 spelling）
3) 宏展开时生成 `PPOrigin` 并挂到新 token 上
4) `PPToken -> Token` 转换：把 `spelling/origin` 搬过去
5) 统一错误打印：实现宏回溯输出
