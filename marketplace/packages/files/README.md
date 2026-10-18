# Files

A file explorer for the ForkMesh desktop, iPhone and Android phone. Install it
from the Market to add **Files** to the phones' Home screen and keep it on the
desktop rail; installing it on one device installs it on your others.

It shows two kinds of places:

- **This device**: the computer's folders on the desktop; on the iPhone, the
  ForkMesh folder (also visible in the Files app) and any folder you add,
  including iCloud Drive and servers connected in the Files app; on Android,
  the ForkMesh folder and any folder you add with the system picker.
- **Network shares**: folders shared over SMB on your local network, such as a
  USB disk your router shares (Router → Storage → Network shares → **Browse
  files**), a NAS, or a shared folder on a Mac or Windows computer. Enter the
  address and share name (or press **Find shares**), then sign in as a guest or
  with a user and password. The password is kept only in the device's secure
  store, and only if you ask.

In every place you can:

- browse with a path bar, sort by name, date or size, filter the folder by
  name, and show or hide hidden files;
- open text and code files in the editor and save them back where they came
  from, view pictures and PDFs, and open anything else in another app;
- create folders and files, rename, delete, copy and move, also between the
  device and a network share;
- upload files into the open folder and download or share files from it.

Network shares must be on your local network. ForkMesh speaks SMB 2.1, which
Samba, ksmbd (OpenWrt), Windows and macOS all accept.
