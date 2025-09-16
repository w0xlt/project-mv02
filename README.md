# mevpool

1. Build and install Bitcoin Core from the PR #30595 head with the kernel lib enabled
```
git clone https://github.com/w0xlt/bitcoin.git
cd bitcoin
git checkout -b kernelApi_input_struct origin/kernelApi_input_struct

cmake -S . -B build \
  -DBUILD_KERNEL_LIB=ON \
  -DBUILD_SHARED_LIBS=ON \
  -DBUILD_GUI=OFF -DBUILD_TESTS=OFF -DBUILD_BENCH=OFF

cmake --build build -j
```
2. `sudo apt install cmake git ninja`
3. `git clone https://github.com/w0xlt/mevpool`
4. [Install VCPKG](https://learn.microsoft.com/en-us/vcpkg/get_started/get-started?pivots=shell-bash)
5. Add the following `CMakeUserPresets.json`:
```json
{
  "version": 2,
  "configurePresets": [
    {
      "name": "default",
      "inherits": "vcpkg",
      "environment": {
        "VCPKG_ROOT": "<vcpkg_path>"
      },
      "cacheVariables": {
        "BITCOIN_DIR": "<bitcoin_path>"
      }
    }
  ]
}
```
6. `cmake --preset=default`
7. `cmake --build build -j (math (nproc) +1)`
8. `./build/bitcoinkernel_http`