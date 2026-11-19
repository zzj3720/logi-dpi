///|
extern "C" fn mousedpi_setup_window(w : Int, h : Int) -> Int = "mousedpi_setup_window"

///|
extern "C" fn mousedpi_init_and_restore() -> Int = "mousedpi_init_and_restore"

///|
extern "C" fn mousedpi_read_state() -> Int = "mousedpi_read_state"

///|
extern "C" fn mousedpi_set_dpi(x : Int, y : Int, lod : Int) -> Int = "mousedpi_set_dpi"

///|
extern "C" fn mousedpi_get_dpi_x() -> Int = "mousedpi_get_dpi_x"

///|
extern "C" fn mousedpi_get_lod() -> Int = "mousedpi_get_lod"

///|
extern "C" fn mousedpi_get_battery() -> Int = "mousedpi_get_battery"

///|
extern "C" fn mousedpi_get_autostart() -> Int = "mousedpi_get_autostart"

///|
extern "C" fn mousedpi_set_autostart(on : Int) -> Int = "mousedpi_set_autostart"

///|
struct AppState {
  mut inited : Bool
  mut win_ready : Bool
  mut connected : Bool
  mut dpi : Int
  mut lod : Int
  mut battery : Int
  mut autostart : Bool
  mut dragging : Bool
}

///|
let state : AppState = {
  inited: false,
  win_ready: false,
  connected: false,
  dpi: 800,
  lod: 2,
  battery: -1,
  autostart: false,
  dragging: false,
}

///|
let slider_min : Double = 200.0

///|
let slider_max : Double = 4000.0

///|
fn sync_from_c(ok : Int) -> Unit {
  if ok != 0 {
    state.connected = true
    state.dpi = mousedpi_get_dpi_x()
    state.lod = mousedpi_get_lod()
    state.battery = mousedpi_get_battery()
  } else {
    state.connected = false
  }
}

///|
fn init_mouse() -> Unit {
  let ok = mousedpi_init_and_restore()
  sync_from_c(ok)
  state.autostart = mousedpi_get_autostart() != 0
}

///|
fn refresh_mouse() -> Unit {
  let ok = mousedpi_read_state()
  sync_from_c(ok)
  state.autostart = mousedpi_get_autostart() != 0
}

///|
fn apply_dpi(target_dpi : Int, target_lod : Int) -> Unit {
  let clamped = if target_dpi < 100 {
    100
  } else if target_dpi > 25600 {
    25600
  } else {
    target_dpi
  }
  let rc = mousedpi_set_dpi(clamped, clamped, target_lod)
  if rc == 0 {
    state.connected = true
    state.dpi = mousedpi_get_dpi_x()
    state.lod = mousedpi_get_lod()
  }
}

///|
fn dpi_to_ratio(dpi : Int) -> Double {
  let r = (dpi.to_double() - slider_min) / (slider_max - slider_min)
  if r < 0.0 {
    0.0
  } else if r > 1.0 {
    1.0
  } else {
    r
  }
}

///|
fn step_btn(c : @ui.Ctx, id : Int, label : String) -> Bool {
  let hov = c.hovered(id)
  let bg : Int64 = if hov { 0x2A303CFFL } else { 0x1E222BFFL }
  let border_c : Int64 = if hov { 0xFFFFFF2EL } else { 0xFFFFFF18L }
  let _ = c.box(
    id~,
    w=@ui.Px(54.0),
    h=@ui.Px(34.0),
    align=@ui.ACenter,
    justify=@ui.JCenter,
    bg~,
    radius=9.0,
    border=1.0,
    border_color=border_c,
    fn() {
      let _ = c.text(label, size=13.0, weight=600, color=0xD0D7DEFFL, tabular=true)
    },
  )
  c.pressed(id)
}

///|
fn preset_pill(c : @ui.Ctx, id : Int, label : String, active : Bool) -> Bool {
  let hov = c.hovered(id)
  let bg : Int64 = if active {
    0xF5F7FAFFL
  } else if hov {
    0x1F242EFFL
  } else {
    0x13161DFFL
  }
  let border_c : Int64 = if active {
    0xFFFFFFFFL
  } else if hov {
    0xFFFFFF2EL
  } else {
    0xFFFFFF1AL
  }
  let fg : Int64 = if active { 0x0B0C0FFFL } else { 0xC9D1D9FFL }
  let _ = c.box(
    id~,
    w=@ui.Fill(1.0),
    h=@ui.Px(38.0),
    align=@ui.ACenter,
    justify=@ui.JCenter,
    bg~,
    radius=999.0,
    border=1.0,
    border_color=border_c,
    shadow=if active { 10.0 } else { 0.0 },
    shadow_color=0xFFFFFF22L,
    fn() {
      let _ = c.text(
        label,
        size=13.5,
        weight=if active { 700 } else { 500 },
        color=fg,
        tabular=true,
      )
    },
  )
  c.pressed(id)
}

///|
fn lod_segment(c : @ui.Ctx, id : Int, label : String, active : Bool) -> Bool {
  let hov = c.hovered(id)
  let bg : Int64 = if active {
    0x2B313DFFL
  } else if hov {
    0x1B1F28FFL
  } else {
    0x00000000L
  }
  let fg : Int64 = if active { 0xF5F7FAFFL } else { 0x8B949EFFL }
  let _ = c.box(
    id~,
    w=@ui.Fill(1.0),
    h=@ui.Px(32.0),
    align=@ui.ACenter,
    justify=@ui.JCenter,
    bg~,
    radius=999.0,
    border=if active { 1.0 } else { 0.0 },
    border_color=0xFFFFFF1CL,
    fn() {
      let _ = c.text(label, size=13.0, weight=if active { 600 } else { 500 }, color=fg)
    },
  )
  c.pressed(id)
}

///|
fn view(c : @ui.Ctx) -> Unit {
  if !state.inited {
    state.inited = true
    init_mouse()
  }
  if !state.win_ready {
    if mousedpi_setup_window(780, 460) != 0 {
      state.win_ready = true
    }
  }

  let (px, py, down) = c.pointer()
  if !down {
    state.dragging = false
  } else {
    match c.laid_out(4001) {
      Some((rx, ry, rw, rh)) => {
        let in_track = px >= rx - 8.0 && px <= rx + rw + 8.0 && py >= ry - 8.0 && py <= ry + rh + 8.0
        if in_track || state.dragging {
          state.dragging = true
          if rw > 1.0 {
            let t_raw = (px - rx) / rw
            let t = if t_raw < 0.0 { 0.0 } else if t_raw > 1.0 { 1.0 } else { t_raw }
            let raw_dpi = slider_min + t * (slider_max - slider_min)
            let snapped = ((raw_dpi / 50.0 + 0.5).to_int()) * 50
            if snapped != state.dpi {
              apply_dpi(snapped, state.lod)
            }
          }
        }
      }
      None => ()
    }
  }

  c.set_clear(0x0B0C0FFFL)

  let _ = c.box(
    dir=@ui.Column,
    w=@ui.Fill(1.0),
    h=@ui.Fill(1.0),
    pad=22.0,
    gap=18.0,
    bg=0x0B0C0FFFL,
    fn() {
      // 1. Top bar
      let _ = c.box(
        dir=@ui.Row,
        w=@ui.Fill(1.0),
        align=@ui.ACenter,
        justify=@ui.JBetween,
        fn() {
          let _ = c.box(dir=@ui.Row, gap=10.0, align=@ui.ACenter, fn() {
            let dot_c : Int64 = if state.connected { 0x22C55EFFL } else { 0xF85149FFL }
            let _ = c.box(
              w=@ui.Px(9.0),
              h=@ui.Px(9.0),
              radius=999.0,
              bg=dot_c,
              shadow=8.0,
              shadow_color=if state.connected { 0x22C55E88L } else { 0xF8514988L },
              fn() {},
            )
            let _ = c.text("PRO X2 SUPERSTRIKE", size=17.5, weight=700, color=0xF5F7FAFFL)
            let _ = c.box(
              padx=9.0,
              pady=4.0,
              radius=6.0,
              bg=0x171A22FFL,
              border=1.0,
              border_color=0xFFFFFF16L,
              fn() {
                let _ = c.text(
                  if state.connected { "HID++ 2.0 直连 · 托盘守护" } else { "未连接" },
                  size=11.0,
                  weight=500,
                  color=0x8B949EFFL,
                )
              },
            )
          })

          let _ = c.box(dir=@ui.Row, gap=8.0, align=@ui.ACenter, fn() {
            if state.connected && state.battery >= 0 {
              let bat_c : Int64 = if state.battery <= 20 { 0xFF7B72FFL } else { 0x3FB950FFL }
              let _ = c.box(
                dir=@ui.Row,
                gap=7.0,
                padx=12.0,
                pady=6.0,
                radius=999.0,
                bg=0x161921FFL,
                border=1.0,
                border_color=0xFFFFFF18L,
                align=@ui.ACenter,
                fn() {
                  let _ = c.box(w=@ui.Px(6.0), h=@ui.Px(6.0), radius=999.0, bg=bat_c, fn() {})
                  let _ = c.text("\{state.battery}%", size=12.5, weight=600, color=0xE6EDF3FFL, tabular=true)
                },
              )
            }
            let ref_hov = c.hovered(1001)
            let _ = c.box(
              id=1001,
              padx=11.0,
              h=@ui.Px(30.0),
              align=@ui.ACenter,
              justify=@ui.JCenter,
              radius=8.0,
              bg=if ref_hov { 0x232834FFL } else { 0x161921FFL },
              border=1.0,
              border_color=if ref_hov { 0xFFFFFF2AL } else { 0xFFFFFF18L },
              fn() {
                let _ = c.text("刷新", size=12.0, weight=500, color=0xC9D1D9FFL)
              },
            )
            if c.pressed(1001) {
              refresh_mouse()
            }
          })
        },
      )

      // 2. Main 2-column body
      let _ = c.box(
        dir=@ui.Row,
        w=@ui.Fill(1.0),
        h=@ui.Fill(1.0),
        gap=18.0,
        fn() {
          // Left Column: Mouse Product Card + Autostart toggle
          let _ = c.box(
            dir=@ui.Column,
            w=@ui.Px(264.0),
            h=@ui.Fill(1.0),
            pad=16.0,
            align=@ui.ACenter,
            justify=@ui.JBetween,
            bg=0x13161DFFL,
            radius=16.0,
            border=1.0,
            border_color=0xFFFFFF18L,
            fn() {
              let _ = c.image(
                "mouse.png",
                w=@ui.Px(148.0),
                h=@ui.Px(222.0),
                bg=0x13161DFFL,
                fade=false,
              )
              let _ = c.box(dir=@ui.Column, w=@ui.Fill(1.0), align=@ui.ACenter, gap=8.0, fn() {
                let _ = c.text("LIGHTSPEED · 自动保存", size=11.5, weight=500, color=0x8B949EFFL)
                let as_hov = c.hovered(1002)
                let as_bg : Int64 = if state.autostart {
                  if as_hov { 0x1B3B28FFL } else { 0x142D1FFFL }
                } else if as_hov {
                  0x222733FFL
                } else {
                  0x1A1E27FFL
                }
                let as_bd : Int64 = if state.autostart { 0x22C55E55L } else { 0xFFFFFF18L }
                let as_dot : Int64 = if state.autostart { 0x22C55EFFL } else { 0x636C76FFL }
                let as_fg : Int64 = if state.autostart { 0x86EFACFFL } else { 0x9DA7B3FFL }
                let _ = c.box(
                  id=1002,
                  dir=@ui.Row,
                  gap=7.0,
                  padx=12.0,
                  h=@ui.Px(30.0),
                  radius=999.0,
                  align=@ui.ACenter,
                  justify=@ui.JCenter,
                  bg=as_bg,
                  border=1.0,
                  border_color=as_bd,
                  fn() {
                    let _ = c.box(w=@ui.Px(6.0), h=@ui.Px(6.0), radius=999.0, bg=as_dot, fn() {})
                    let _ = c.text(
                      if state.autostart { "开机自启：已开启" } else { "开机自启：已关闭" },
                      size=11.5,
                      weight=500,
                      color=as_fg,
                    )
                  },
                )
                if c.pressed(1002) {
                  let next = if state.autostart { 0 } else { 1 }
                  state.autostart = mousedpi_set_autostart(next) != 0
                }
              })
            },
          )

          // Right Column: Control Stack
          let _ = c.box(
            dir=@ui.Column,
            w=@ui.Fill(1.0),
            h=@ui.Fill(1.0),
            justify=@ui.JBetween,
            gap=16.0,
            fn() {
              // Hero DPI + Ticked Slider Card
              let _ = c.box(
                dir=@ui.Column,
                w=@ui.Fill(1.0),
                pad=20.0,
                gap=16.0,
                bg=0x13161DFFL,
                radius=16.0,
                border=1.0,
                border_color=0xFFFFFF18L,
                fn() {
                  let _ = c.box(
                    dir=@ui.Row,
                    w=@ui.Fill(1.0),
                    align=@ui.ACenter,
                    justify=@ui.JBetween,
                    fn() {
                      let _ = c.box(dir=@ui.Row, gap=8.0, align=@ui.AEnd, fn() {
                        let _ = c.text(
                          "\{state.dpi}",
                          size=46.0,
                          weight=700,
                          color=0xF5F7FAFFL,
                          tabular=true,
                        )
                        let _ = c.box(pb=8.0, fn() {
                          let _ = c.text("DPI", size=14.5, weight=500, color=0x8B949EFFL)
                        })
                      })
                      let _ = c.box(dir=@ui.Row, gap=8.0, align=@ui.ACenter, fn() {
                        if step_btn(c, 1101, "-50") {
                          apply_dpi(state.dpi - 50, state.lod)
                        }
                        if step_btn(c, 1102, "+50") {
                          apply_dpi(state.dpi + 50, state.lod)
                        }
                      })
                    },
                  )

                  let target_pct = dpi_to_ratio(state.dpi) * 100.0
                  let anim_pct = if state.dragging {
                    target_pct
                  } else {
                    c.spring("dpi_s", target_pct, from=target_pct, stiffness=320.0, damping=28.0)
                  }
                  let clamped_pct = if anim_pct < 0.0 {
                    0.0
                  } else if anim_pct > 100.0 {
                    100.0
                  } else {
                    anim_pct
                  }

                  let _ = c.box(dir=@ui.Column, w=@ui.Fill(1.0), gap=6.0, fn() {
                    let _ = c.box(
                      id=4001,
                      w=@ui.Fill(1.0),
                      h=@ui.Px(22.0),
                      align=@ui.ACenter,
                      justify=@ui.JStart,
                      dir=@ui.Row,
                      fn() {
                        let _ = c.box(
                          w=@ui.Fill(1.0),
                          h=@ui.Px(6.0),
                          bg=0x232833FFL,
                          radius=999.0,
                          dir=@ui.Row,
                          align=@ui.ACenter,
                          fn() {
                            let _ = c.box(
                              w=@ui.Pct(clamped_pct),
                              h=@ui.Px(6.0),
                              bg=0x8CEEFFFFL,
                              radius=999.0,
                              shadow=10.0,
                              shadow_color=0x38BDF877L,
                              fn() {},
                            )
                            let _ = c.box(
                              w=@ui.Px(18.0),
                              h=@ui.Px(18.0),
                              ml=-9.0,
                              bg=0xFFFFFFFFL,
                              radius=999.0,
                              shadow=8.0,
                              shadow_color=0x00000099L,
                              fn() {},
                            )
                          },
                        )
                      },
                    )

                    let _ = c.box(
                      dir=@ui.Row,
                      w=@ui.Fill(1.0),
                      h=@ui.Px(10.0),
                      align=@ui.AStart,
                      justify=@ui.JBetween,
                      fn() {
                        for i = 0; i <= 24; i = i + 1 {
                          let major = i % 6 == 0
                          let _ = c.box(
                            w=@ui.Px(1.0),
                            h=@ui.Px(if major { 9.0 } else { 5.0 }),
                            bg=if major { 0xFFFFFF44L } else { 0xFFFFFF1EL },
                            fn() {},
                          )
                        }
                      },
                    )
                  })
                },
              )

              // Quick Presets Row
              let _ = c.box(dir=@ui.Column, w=@ui.Fill(1.0), gap=9.0, fn() {
                let _ = c.text("常用档位", size=12.5, weight=500, color=0x8B949EFFL)
                let presets = [400, 800, 1200, 1600, 3200]
                let _ = c.box(dir=@ui.Row, w=@ui.Fill(1.0), gap=8.0, fn() {
                  for i = 0; i < presets.length(); i = i + 1 {
                    let v = presets[i]
                    if preset_pill(c, 2000 + i, "\{v}", state.dpi == v) {
                      apply_dpi(v, state.lod)
                    }
                  }
                })
              })

              // LOD Segmented Control
              let _ = c.box(dir=@ui.Column, w=@ui.Fill(1.0), gap=9.0, fn() {
                let _ = c.text("抬起距离 LOD", size=12.5, weight=500, color=0x8B949EFFL)
                let _ = c.box(
                  dir=@ui.Row,
                  w=@ui.Fill(1.0),
                  pad=4.0,
                  gap=4.0,
                  bg=0x13161DFFL,
                  radius=999.0,
                  border=1.0,
                  border_color=0xFFFFFF18L,
                  fn() {
                    if lod_segment(c, 3001, "低", state.lod == 1) {
                      apply_dpi(state.dpi, 1)
                    }
                    if lod_segment(c, 3002, "中", state.lod == 2) {
                      apply_dpi(state.dpi, 2)
                    }
                    if lod_segment(c, 3003, "高", state.lod == 3) {
                      apply_dpi(state.dpi, 3)
                    }
                  },
                )
              })
            },
          )
        },
      )
    },
  )
}

///|
fn main {
  @ui.run("PRO X2 SUPERSTRIKE", view)
}
