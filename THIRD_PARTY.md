# Third-party components

autojam's own code is MIT (see `LICENSE`). A release bundles and downloads other people's work:

| component | what | licence / terms |
|---|---|---|
| [llamatalk](https://github.com/mercurious/llamatalk) | `llamatalk/`: `llama-server` (llama.cpp) and `tts-server` (pockettts.cpp) at pinned commits, and `models.sh` | MIT; see its `THIRD_PARTY.md` |
| [llama.cpp](https://github.com/ggml-org/llama.cpp) | the language model server | MIT |
| [pockettts.cpp](https://github.com/ServeurpersoCom/pockettts.cpp) | the speech server | MIT |
| Gemma 4 E4B (QAT Q4_0 GGUF, and its audio projector) | the language model, downloaded by `models.sh` from Hugging Face | [Gemma Terms of Use](https://ai.google.dev/gemma/terms) |
| Pocket TTS English (Kyutai) | the voice, downloaded by `models.sh` | CC-BY-4.0 with Kyutai's prohibited uses |
| Liberation Sans | `assets/fonts/` | SIL Open Font License 1.1 |
| SDL2, SDL2_ttf, libpulse-simple | linked at build time; the device ships its own copies | zlib, zlib, LGPL-2.1 |

No model is redistributed in a release: each is fetched from its own model card by the installer.
