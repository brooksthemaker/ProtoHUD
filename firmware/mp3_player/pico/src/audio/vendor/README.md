# Vendored audio decoder headers

Copy the following header-only libraries here before building:

## minimp3 (MP3 decoder)
- Source: https://github.com/lieff/minimp3
- Files needed: `minimp3.h`, `minimp3_ex.h`
- License: CC0

```sh
curl -L https://raw.githubusercontent.com/lieff/minimp3/master/minimp3.h -o minimp3.h
curl -L https://raw.githubusercontent.com/lieff/minimp3/master/minimp3_ex.h -o minimp3_ex.h
```

## dr_flac (FLAC decoder)
- Source: https://github.com/mackron/dr_libs
- File needed: `dr_flac.h`
- License: MIT / public domain

```sh
curl -L https://raw.githubusercontent.com/mackron/dr_libs/master/dr_flac.h -o dr_flac.h
```
