#pragma once

// `epd` command, the e-ink panel:
//   epd [info]                       controller, probe bytes, stock screenType, refreshes
//   epd test [pattern] [full|fast]   draw a test frame (default: orient, full)
//                                    orient: border, a big square top left, a small one
//                                            top right, a bar along the bottom
//                                    corners: a 60 px square in each corner, a notch
//                                    edges: 2 px lines every 4 px from each edge
//                                           (count the visible ones: the inset)
//                                    checker: 40 px squares; black; white
//   epd sleep                        deep sleep (the picture stays)

namespace console {

int CmdEpd(int argc, char **argv);

}  // namespace console
