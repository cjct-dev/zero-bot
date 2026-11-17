# Third-party notices

Zero Bot itself is under the MIT licence (`LICENSE`). It also contains, or is built from, the
following. Their notices are reproduced here as their licences ask.

## Bundled or linked

- **nlohmann/json** (`third_party/nlohmann/json.hpp`): MIT License, Copyright (c) 2013-2023 Niels
  Lohmann.
- **LVGL**: MIT License, Copyright (c) 2025 LVGL Kft.

## Ported (re-written in C++, not copied)

### Fire TV driver (`src/smarthome/drivers/firetv.cpp`)

Parts of the protocol handling follow these two projects:

- firehose-remote, github.com/AustrianCanvasSociety/firehose-remote: MIT License, Copyright (c)
  2026 AustrianCanvasSociety
- hms-firetv, github.com/hms-homelab/hms-firetv: MIT License, Copyright (c) 2026 HMS Homelab

Permission is hereby granted, free of charge, to any person obtaining a copy of this software and
associated documentation files (the "Software"), to deal in the Software without restriction,
including without limitation the rights to use, copy, modify, merge, publish, distribute,
sublicense, and/or sell copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all copies or
substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT
NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM,
DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT
OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

### HomeKit driver (`src/smarthome/drivers/homekit.cpp`)

Parts of the HomeKit Accessory Protocol handling follow these two projects, both under the Apache
License, Version 2.0 (full text in `Apache-2.0.txt` beside this file). Neither has a NOTICE file.

- aiohomekit, github.com/Jc2k/aiohomekit: Copyright 2019 aiohomekit team. Licensed under the
  Apache License, Version 2.0.
- HomeKit ADK, github.com/apple/HomeKitADK: Copyright (c) 2015-2019 The HomeKit ADK Contributors.
  Licensed under the Apache License, Version 2.0.

The C++ code in `homekit.cpp` is a re-write over libcrypto; the file lists what each part
follows. "HomeKit" is a trademark of Apple Inc. Zero Bot is not made or endorsed by Apple or
Amazon.
