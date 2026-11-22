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

### Hue driver (`src/smarthome/drivers/hue.cpp`)

Parts of the Hue Bridge protocol handling follow these two projects, both under the Apache License,
Version 2.0 (full text in `Apache-2.0.txt` beside this file). Neither has a NOTICE file.

- aiohue, github.com/home-assistant-libs/aiohue: Licensed under the Apache License, Version 2.0;
  its copyright notice is in its LICENSE file.
- openhue-api, github.com/openhue/openhue-api (an OpenAPI description of the bridge's CLIP version
  2 interface): Licensed under the Apache License, Version 2.0.

The C++ code in `hue.cpp` is a re-write; nothing was copied, and the file lists what each part
follows. Zero Bot is not made or endorsed by Signify, the maker of Philips Hue.

### Tapo transport (`src/smarthome/tapo.cpp`)

The TPAP login follows the `tapo` crate, github.com/mihai-dinculescu/tapo, version 0.13.0: the
SPAKE2+ prover is a port of `tapo/src/api/protocol/tpap_spake2p.rs`, and the checks on the
device's answers follow `tapo/src/api/protocol/tpap_protocol.rs`. The simulated plug in
`tests/mocks/mock_tapo.py` (a test tool, not installed) checks its own code against two vectors
from the crate. The crate's licence:

```
MIT License

Copyright (c) 2022-2025 Mihai Dinculescu

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

No code was taken from python-kasa (GPL-3.0), which was read for protocol facts only.

### WiZ driver (`src/smarthome/drivers/wiz.cpp`)

The protocol handling (the methods, how a module name says what a device can do, the colour
conversion to the red, green, blue and white LEDs, signed requests) follows pywizlight,
github.com/sbidy/pywizlight: MIT License, Copyright (c) 2020 Stephan Traub. The simulated device in
`tests/mocks/wiz/` follows the answers recorded in its tests. The MIT permission notice and
disclaimer are as quoted above for the Fire TV driver.

## In the tests (not installed)

### Ed25519 in the simulated HomeKit accessory (`tests/mocks/homekit/mock_homekit.py`)

The point arithmetic, point encoding, signing and verifying follow the Python code of RFC 8032
section 6, a code component of that RFC, used under the Simplified BSD License as section 4.e of the
IETF Trust's Legal Provisions Relating to IETF Documents describes. The file keeps this notice:

```
Copyright (c) 2017 IETF Trust and the persons identified as authors of the code. All rights
reserved.

Redistribution and use in source and binary forms, with or without modification, are
permitted provided that the following conditions are met:
- Redistributions of source code must retain the above copyright notice, this list of
  conditions and the following disclaimer.
- Redistributions in binary form must reproduce the above copyright notice, this list of
  conditions and the following disclaimer in the documentation and/or other materials
  provided with the distribution.
- Neither the name of Internet Society, IETF or IETF Trust, nor the names of specific
  contributors, may be used to endorse or promote products derived from this software
  without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS
OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE
GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED
OF THE POSSIBILITY OF SUCH DAMAGE.
```

## Read, not ported

No code was copied from these; they were read for protocol facts or to check behaviour, and need no
notice: python-kasa (GPL-3.0; Kasa and Tapo), tplink-smarthome-api (MIT; Kasa), softScheck's write-up
of the Kasa protocol, HAP-NodeJS (Apache-2.0; HomeKit), openHAB's Hue binding (EPL-2.0) and aioshelly
(Apache-2.0; Shelly).
