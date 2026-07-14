# Changelog

## 6.0-2

- Moved gettext catalogs to `/usr/share/locale/<lang>/LC_MESSAGES/flitz-toolkit.mo`.
- Changed the gettext domain from `flitz` to `flitz-toolkit`.
- Completed all 101 messages in the 11 included language catalogs.
- Added a simple GTK3 About dialog to both applications.
- Relicensed the C version under GNU GPL version 3 or later.
- Kept the intentional ROX `.desktop` launcher link unchanged.

## 6.0-1

- Rewritten in C with GTK3.
- Removed Python, Tkinter, Pillow and tkinterdnd2 runtime requirements.
- Preserved compressor and extractor as separate applications.
- Preserved native file opening and the intentional ROX `.desktop` link.
- Added native GTK drag and drop.
- Kept all original compression formats and special extraction handlers.
- Made Recursive, Keep structure and Overwrite functional.
- Added safe staging before extracted files are written to the selected output.
- Added progress parsing, live console output and threaded operations.
- Added translations for the Essora language set with English fallback.
- Fixed PET extraction by removing and validating Puppy's 32-byte MD5 footer
  before passing the compressed tar payload to `tar`.
- Renamed the Debian package to `flitz-toolkit`.
