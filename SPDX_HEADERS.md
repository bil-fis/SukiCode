# SPDX License Header Templates

## 编译器/运行时文件 (Apache-2.0 WITH Runtime-exception)

### SukiCode 源文件 (.suki)

```suki
// SPDX-License-Identifier: Apache-2.0 WITH Runtime-exception
// Copyright 2026 SukiCode Contributors
// Licensed under the Apache License, Version 2.0 with Runtime Library Exception.
// See LICENSE_Apache-2.0.txt and RUNTIME_EXCEPTION.txt for details.
```

### C/C++ 头文件 (.h)

```cpp
/*
 * SPDX-License-Identifier: Apache-2.0 WITH Runtime-exception
 * Copyright 2026 SukiCode Contributors
 * Licensed under the Apache License, Version 2.0 with Runtime Library Exception.
 * See LICENSE_Apache-2.0.txt and RUNTIME_EXCEPTION.txt for details.
 */
#pragma once
```

### C++ 源文件 (.cpp)

```cpp
/*
 * SPDX-License-Identifier: Apache-2.0 WITH Runtime-exception
 * Copyright 2026 SukiCode Contributors
 * Licensed under the Apache License, Version 2.0 with Runtime Library Exception.
 * See LICENSE_Apache-2.0.txt and RUNTIME_EXCEPTION.txt for details.
 */
```

---

## 工具文件 (MIT)

### SukiCode 源文件 (.suki)

```suki
// SPDX-License-Identifier: MIT
// Copyright 2026 SukiCode Contributors
// Licensed under the MIT License.
// See LICENSE_MIT.txt for details.
```

### C/C++ 头文件 (.h)

```cpp
/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 SukiCode Contributors
 * Licensed under the MIT License.
 * See LICENSE_MIT.txt for details.
 */
#pragma once
```

### C++ 源文件 (.cpp)

```cpp
/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 SukiCode Contributors
 * Licensed under the MIT License.
 * See LICENSE_MIT.txt for details.
 */
```

---

## 文档文件 (CC-BY-4.0)

### Markdown 文档 (.md)

```markdown
<!--
SPDX-License-Identifier: CC-BY-4.0
Copyright 2026 SukiCode Contributors
Licensed under the Creative Commons Attribution 4.0 International License.
See LICENSE_CC-BY-4.0.txt for details.
-->
```

---

## JSON 配置文件

JSON 不支持注释。使用以下方案之一：

1. **添加 `_license` 字段**（推荐）:
   ```json
   {
     "_license": "Apache-2.0 WITH Runtime-exception",
     "name": "my-config",
     ...
   }
   ```

2. **使用 `.reuse/dep5` 文件**批量标注（见下文）。

---

## 使用说明

- 编译器核心（`src/compiler/`）、运行时（`src/runtime/`）、标准库（`src/stdlib/`）使用 **Apache-2.0 WITH Runtime-exception**
- 工具（`tools/sukipm/`、`tools/suki-lsp/`、`tools/suki-fmt/`、`tools/suki-doc/`）使用 **MIT**
- 文档（`docs/`、`*.md`）使用 **CC-BY-4.0**
- 示例代码（`examples/`、`moduleTest/`）使用 **MIT**
