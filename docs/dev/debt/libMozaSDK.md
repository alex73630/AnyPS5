# libMozaSDK technical debt

### Functional

- [libMozaSDK](../../../core/libs/prx/libMozaSDK/Export.cpp) provides no SDK exports. PPSA01317 version 01.710.000 declares the library as a dependency without importing any symbols from it or supplying the module in its extracted files. The library contains only the throwing `APS5_DUMMY_FUN` placeholder, as in the GT7 integration fork. SDK initialization, device handling and exports are unknown; this does not implement MOZA wheel support. Any SDK symbol requested by another title remains unresolved.
