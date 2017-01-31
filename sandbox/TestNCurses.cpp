#include "NCursesOverlay.hpp"

using namespace et;

int main(int argc, char** argv) {
  srand(1);
  google::InitGoogleLogging(argv[0]);
  gflags::ParseCommandLineFlags(&argc, &argv, true);

  {
    NCursesOverlay overlay;
    shared_ptr<NCursesWindow> popupWindow;
    {
      TerminalInfo terminfo;
      terminfo.set_id("popup");
      terminfo.set_height(7);
      terminfo.set_width(41);
      terminfo.set_row(overlay.rows() / 2 - 3);
      terminfo.set_column(overlay.cols() / 2 - 20);

      popupWindow = overlay.createWindow(terminfo, true);
      popupWindow->drawTextCentered("Please wait, reconnecting...", 3);
      popupWindow->drawTextCentered("Please wait, reconnecting...", 4);
    }
    overlay.refresh();
    cout << "This stdout should be delayed" << endl;
    sleep(3);
  }
  cout << "This stdout should be shown immediately" << endl;
  return 0;
}
