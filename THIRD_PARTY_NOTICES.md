# Third-party notices

GoldenEye Metal is a multi-license source tree. A license applies only to the files or portions
identified by that license; no single license overrides the terms of bundled third-party work.

## Runtime and static-recompilation toolchain

The root runtime and toolchain are distributed under the BSD 3-Clause license in [LICENSE](LICENSE).
That file also contains the required notice for portions derived from the Xenia project. The same
BSD notice applies to the Xenia-derived mouse-input portion in the vendored game integration.

## Vendored game integration

Original integration code under `vendor/GoldenEye-Recomp/` is distributed under the Unlicense in
`vendor/GoldenEye-Recomp/LICENSE`, except for the portions identified below.

### Community Edition patch data and logic

Portions of `vendor/GoldenEye-Recomp/src/ge_ce_patches.cpp`,
`vendor/GoldenEye-Recomp/src/ge_hooks.cpp`, and
`vendor/GoldenEye-Recomp/src/ce_patches/` are derived from BeanTools and are used under the MIT
License:

```text
Copyright (c) 2025 Carnivorous

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

## Source dependencies

Dependencies under `thirdparty/` are pinned Git submodules or vendored source packages. Their own
license and notice files remain authoritative. Binary distributors must reproduce all notices
required by the dependencies they include.

## Excluded rights

No license in this repository grants rights to proprietary game executables, generated game code,
audio, artwork, extracted assets, or trademarks. Those materials are not distributed here.
