# wtfd

wtfd is a network service that wonders "wtf". It runs on port 23206.

Build:

    make

Run:

    ./wtfd

Use (as a client):

    nc localhost 23206 < /dev/null

It has Debian/Ubuntu packaging and includes a systemd service file. To build the
package:

    gbp buildpackage -uc -us

To install the package:

    sudo dpkg -i ../wtfd_*.deb

The service will start automatically. To check its status:

    systemctl status wtfd
