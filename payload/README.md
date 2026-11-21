# Safe Connect payload

`safe_connect.elf` blocks PS5 system updates on the console itself. Send it to the payload
loader (port 9021, the same way as any other payload). It does exactly what the Safe Connect app
does over FTP:

1. It deletes a downloaded update (any `PS5UPDATE*` file) in `/update`.
2. It makes a folder at `PS5UPDATE.PUP` and `PS5UPDATE.PUP.net.temp`, so a download has nowhere
   to go.
3. It puts a read-only `prospero-win-update-blocker` file inside each folder and makes the
   folders read-only.

It then shows a notification with the result and exits. Running it again is harmless: anything
already blocked stays blocked.

| Payload | What it does |
|---|---|
| `safe_connect.elf` | Blocks updates. |
| `safe_connect_unblock.elf` | Removes the block. A folder that holds anything else is left alone. |
| `safe_connect_check.elf` | Only reports, in a notification, whether updates are blocked. |

The Safe Connect app (FTP) and the payload use the same folder names, so each one can see and
undo the other's block.

Each payload also sends a one-line report to `127.0.0.1:9077` if something is listening there.
The format is `SAFECONNECT <mode> BLOCKED|OPEN /update/<name>=blocked|open|update-file ...`.

## Build
In WSL: `bash build-safe-connect-elf.sh`. It uses the PS5 payload SDK pinned at `a7685ce`.
`make linux_test` builds Linux versions that work under `./test-root/update`, for testing on a PC.
