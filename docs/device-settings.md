# Device settings

The System page reads supported settings and choices from OpenHD over MAVLink FTP and uses PARAM_EXT for values and writes. Camera, radio and recording controls consume the same runtime metadata. Local display and OSD preferences remain local.

The canonical catalog, JSON editing contract, tests and upstream proposal are documented in the OpenHD repository at OpenHD/settings/README.md. Run its settings/tools/sync_clients.py with --glide pointing to this checkout when updating shared transport headers. No Glide enum table is required for ordinary new backend parameters.
