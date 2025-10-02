# mevpool

1. Build and install Bitcoin Core from the PR #30595 head with the kernel lib enabled
```
git clone https://github.com/TheCharlatan/bitcoin.git
cd bitcoin
git checkout -b kernelApi_outpoints origin/kernelApi_outpoints

cmake -S . -B build \
  -DBUILD_KERNEL_LIB=ON \
  -DBUILD_SHARED_LIBS=OFF \
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

9. How to test ?

```bash
$ ./build/bin/bitcoin-cli  getrawmempool true | jq -r 'to_entries[] | select((.value.depends|length)==0) | .key'
...
aa5dc166b9944e5336e1cb72f2eef3416f7fd6ae3aafbb36a9b086b58f19414c
70dcc87e413ad14c2747ea04739b32e2cde24ae49ca8fbae87892bba94a1f10a

# Choose a TXID
$ TXID=bd9d86a4f19d1049de5f6a67b5468f7755fb34aaa8ba3bd38833fe916eef01b2
$ RAW_TX=$(./build/bin/bitcoin-cli getrawtransaction "$TXID")

# Test TX
$ curl -s http://127.0.0.1:8080/verify \
  -H "Content-Type: application/json" \
  -d "{\"tx_hex\":\"$RAW_TX\"}"
```

