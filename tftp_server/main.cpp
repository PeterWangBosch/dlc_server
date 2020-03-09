#include "include/tftp_server.h"

int main() {
  TFTP server(69);

  server.start();
  while (server.run() >= 0);
  server.stop();

  return 0;
}
