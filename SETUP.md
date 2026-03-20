# DSVP Dependency Setup

Detailed setup instructions for building DSVP from source. See the [README](README.md) for a quick-start overview.

## Steam Deck
This all needs to be updated to be Deck specific

---

## Debug Build

Both platforms support a debug target:

```bash
make debug          # Linux
mingw32-make debug  # Windows
```

This adds `-g -DDSVP_DEBUG`, which enables GPU validation layers, console output, verbose FFmpeg logging, and writes `dsvp.log` to the working directory.
