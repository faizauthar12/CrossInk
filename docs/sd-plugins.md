# SD card plugins

CrossInk can load plugin interfaces in its web portal and browse declarative
plugin catalogs on the reader. JavaScript runs in the connected browser;
`device.json` describes requests and lists that the firmware understands.
No JavaScript engine runs on the reader.

This first integration supports browser interfaces, browser action jobs,
JSON/XML catalogs, authentication, book downloads, and bundle installation.
Firmware event subscriptions and encrypted, expiring loans are not supported.
`/api/crypto` and `/api/book-key` return an explicit unsupported error. Plugins
that require those features need a later integration.

## Installation and discovery

Copy a plugin folder onto the SD card under one of these roots, in priority order:

1. `/.crosspoint/plugins/<name>`
2. `/plugins/<name>`
3. `/.plugins/<name>`

An earlier root wins when folders have the same name. Use simple folder names
such as `my-library`; names cannot contain separators, parent references, FAT
short aliases, or leading/trailing spaces. Folder names are limited to 80 bytes.
The picker discovers up to 16 plugins while inspecting at most 128 entries.

A browser plugin contains `plugin.js`. Optional `manifest.json` supplies its
`title` and `mount` (`settings` by default, or `files`). Open Home → File Transfer
on the reader, join a network, and visit the reader's web portal from a browser.
The plugin appears on the matching Settings or Files page.

A device catalog contains `device.json`. Open **Plugins** on Home in any theme
(shown when plugins are installed), or **Settings → System → Plugins** to select
it. OPDS Browser is available under **Home → File Transfer**. Browser-only
and background-only manifests can appear in this list, but do not open a device
catalog. Read their setup instructions in the browser or on your computer.

Install plugins you trust: their browser scripts run with the web portal's
permissions. Plugin file operations cannot access CrossInk's Wi-Fi, OPDS, or
KOReader credential stores, including backups and legacy Wi-Fi credentials.
Showing hidden files does not expose those stores. Other plugin tokens and
configuration are ordinary files on the SD card.

## Browser interface

Register a renderer in `plugin.js`:

```js
CrossPoint.registerPlugin(async (container, api) => {
  const title = document.createElement('h2');
  title.textContent = 'My library';
  container.append(title);
  // api.name and api.dir identify this installation.
});
```

| API | Purpose |
| --- | --- |
| `api.relay(method, url, headers, body)` | Request HTTP(S) through the device; returns `{status, headers, body}`. |
| `api.fetchToSd(url, dest, headers)` | Download to SD in resumable segments, publishing only a complete file. |
| `api.writeFile(path, base64)` | Write a binary or text file through multipart upload. |
| `api.pluginFile(filename)` | URL for a file in this plugin's folder. |
| `api.registerAction(name, fn)` | Register a browser action callable through the job queue. |

Relay bodies are limited to 32 KiB; larger downloads use `fetchToSd`. File uploads
are limited to 256 KiB. Plugin requests reject HTTP framing headers and control
characters. Credentials are not forwarded to another origin on redirects or
through remote catalog links. Network requests use the existing SecureNet TLS
configuration; no new certificate trust policy is introduced by this feature.

Replacing a book clears its cached content. Reading preferences and progress
continue to use CrossInk's existing stores. Existing file-manager rename and move
operations keep their reader-state migration behavior.

## Device catalog example

```json
{
  "title": "My library",
  "description": "Download books",
  "config": { "file": "config.json" },
  "token": { "file": "token.json", "path": "access_token" },
  "browse": {
    "url": "https://library.example/books?page={page}&limit={limit}",
    "headers": { "Authorization": "Bearer {token}" },
    "items": "books",
    "fields": { "title": "title", "author": "author", "id": "id", "url": "url" },
    "page_size": 8
  },
  "download": {
    "url": "https://library.example/books/{id}/file",
    "headers": { "Authorization": "Bearer {token}" },
    "dest_dir": "/Books",
    "filename": "{title}.epub"
  }
}
```

Omit `token` for a public catalog. Token and configuration filenames relative to
the manifest live inside the plugin folder. Absolute paths must pass the same
credential protection as browser file operations.

Field selectors use dotted JSON paths, including array indexes such as
`authors.0.name`. `browse.items` is empty for a root array. Requests support
`method`, `headers`, and `body`. Templates support `{token}`, `{cfg.KEY}`, `{page}`,
`{limit}`, `{query}` (URL encoded), and `{query_raw}`. Download templates also
support `{id}`, `{title}`, `{author}`, and `{url}`.

Optional `browse.lists` contains named `{title, url, body}` views.
`browse.search` overrides `url` and/or `body` for search. A `download.url_path`
selects a URL from an API response; credentials used for that API request are
not sent to the returned file URL. `download.username` and `download.password`
provide Basic authentication for a direct file request.

For XML catalogs set `browse.format` to `xml`, `browse.item` to the repeated
element's local name, and field selectors to an element, `element@attribute`,
or `@attribute`. Optional `container_element`, `skip_self`, `resolve_urls`, and
`extensions` support directory listings. Directory links must keep the manifest's
origin, including HTTPS and port.

A flat config file can override the destination with `{"dest_dir":"/Books"}`.
`/` selects the SD root. Filenames retain CrossInk's normal sanitation.

## Sign-in

Plugins can save their token through the browser interface, or declare an `auth`
object. `type: "password"` uses `request` and `token_path` to mint a token from
configuration values. `type: "device_code"` uses `request` to obtain a code and
verification URL, then `poll` to await authorization. Response selectors include
`code_path`, `verify_url_path`, `device_code_path`, `interval_path`, `expires_path`,
`token_path`, and `error_path`. The reader shows the code and QR during sign-in.
Expired password tokens receive one refresh attempt. Failed token writes retain
the previous token.

## Bundles and resource limits

A download may declare `bundle: {"base":"base", "files":"files", "subdir":"{id}"}`.
The named item fields supply a base URL and array of relative file paths. The
subdirectory must be one valid folder name. A bundle can contain up to 64 files.

Updates are assembled in a hidden sibling folder, preserving existing local
configuration. After all files arrive, a journal records the folder swap. Boot
recovery retains either the prior installation or the completed replacement.
Copying the prior folder is limited to 128 entries and eight nested levels.
Keep downloaded books outside the plugin installation folder.

Manifests are limited to 8 KiB; picker titles and descriptions accept at most 128
and 256 bytes respectively. Oversized picker fields use the folder name or no
description. Token and configuration files are limited to 2 KiB. JSON pages contain at most 16 visible rows; XML
lists use the SDK's 200-row limit. A list requires enough free heap for its row
array plus 64 KiB of parser and UI headroom; otherwise it shows a memory error. Captured row text is limited to 24 KiB per
response. Catalog responses stream through an SD temporary file and are limited
to 1 MiB; oversized or malformed responses show a feed error. HTTP response
headers are limited to 64 fields and 8 KiB. These bounds keep catalog input from
growing memory without limit on button devices without PSRAM.

## Browser action jobs

An external client may POST `{plugin, action, args}` to `/api/plugin-jobs`, then
poll `/api/plugin-jobs/status?id=<id>`. Jobs run only while a browser hosts the
plugin on Files, Settings, or `/plugins-run`. The device has six job slots.
Running claims have leases; a lost browser may cause an action to run again, so
actions should tolerate retries. Jobs are not persisted across firmware restart.
