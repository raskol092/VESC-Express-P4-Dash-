;;; nav_test.lisp -- автономный тест навигации, жестов и плавности UI (без редактора).
;;;
;;; Загрузка: VESC Tool -> Lisp -> открыть этот файл -> Upload. Импортов нет, всё в одном файле.
;;; Нужна прошивка с мостом r57+ (lv-touch-hook, lv-touch-get, lv-event-wait, lv-perf из 13 элементов).
;;;
;;; Как устроено:
;;;   * Движение за пальцем (листание страниц, шторка, список Main Screens) делает сам LVGL в C:
;;;     это прокручиваемые контейнеры с привязкой (snap). Lisp в покадровом движении не участвует,
;;;     поэтому оно идёт с частотой отрисовки и не зависит от скорости Lisp.
;;;   * Решения принимает Lisp: один общий обработчик касаний (lv-touch-hook) получает каждое
;;;     нажатие/отпускание с координатами, определяет зону начала, направление, удержание
;;;     и переключает состояния. Переходы запускаются кодом (scroll-to с анимацией, screen-load-anim).
;;;   * Дизайн (раздел 4) отделён от навигации (разделы 1-3, 5-7): навигация знает только таблицу
;;;     DESIGN -- для каждого Main Screen три функции, строящие содержимое страниц, и миниатюру.
;;;
;;; Жесты:
;;;   влево / вправо по странице      -> следующая / предыдущая страница (ровно 3, без новых)
;;;   вниз от верхнего края (Main 1)   -> шторка, идёт за пальцем; вверх по шторке -> закрыть
;;;   вниз по странице                 -> другой Main Screen (Main 1 <-> Main 2)
;;;   вверх от нижнего края, коротко   -> другой Main Screen (анимация вверх)
;;;   вверх от нижнего края + удержать -> список Main Screens; тап по карточке -> открыть;
;;;                                       свайп вниз по списку -> закрыть

;; ======================================================================= 1. параметры

(def SW (lv-hor-res))
(def SH (lv-ver-res))

(def EDGE-TOP 30)        ; полоса у верхнего края, откуда тянется шторка, px
(def EDGE-BOT 50)        ; полоса у нижнего края для жеста "снизу вверх", px
(def SWIPE-MIN 70)       ; минимальный путь свайпа, px
(def HOLD-UP 60)         ; насколько поднять палец перед удержанием, px
(def HOLD-STILL 14)      ; палец считается неподвижным, если сдвинулся меньше, px
(def HOLD-TIME 0.30)     ; время неподвижности для "удержания", с
(def ANIM-MS 320)        ; длительность переходов между Main Screens, мс
(def DRAWER-H 300)       ; высота шторки, px
(def HANDLE-H 30)        ; язычок шторки (полоса у верхнего края), px
(def MAIN-COUNT 2)
(def PAGE-COUNT 3)
(def MAIN-HAS-DRAWER (list t nil))   ; шторка только на Main Screen 1

;; ======================================================================= 2. состояние

(def main-screen 0)          ; 0 / 1
(def page 0)                 ; 0 / 1 / 2 -- страница текущего Main Screen
(def page-of (list 0 0))     ; запомненная страница каждого Main Screen
(def drawer-open nil)
(def app-switcher-open nil)
(def gesture-active nil)
(def gesture-zone 'none)     ; top / bottom / body / overlay
(def gesture-start-x 0)
(def gesture-start-y 0)
(def gesture-current-x 0)
(def gesture-current-y 0)
(def gesture-start-time 0)
(def gesture-hold nil)
(def gesture-still-x 0)      ; где палец остановился и когда
(def gesture-still-y 0)
(def gesture-still-time 0)
(def nav-busy-time 0)        ; идёт переход: жесты не принимаются
(def nav-busy-for 0.0)
(def switcher-open-time 0)

;; объекты (заполняются при постройке)
(def mains nil)              ; ((screen pages-container (dot0 dot1 dot2)) ...)
(def drawer nil)             ; хост шторки
(def drawer-spacer nil)
(def switcher nil)           ; хост списка Main Screens
(def switcher-cards nil)     ; ((card-obj . index) ...)
(def switcher-row nil)
(def home-pill nil)
(def perf-label nil)         ; счётчик кадров поверх всего (системный слой)
(def perf-t 0)
(def drawer-mode-btns nil)
(def speed-ring nil)
(def speed-label nil)
(def speed-v 0.0)
(def max-speed-label nil)
(def uptime-label nil)
(def uptime-t0 0)
(def pill-up 0)             ; подъём полоски внизу
(def live-t 0)
(def slow-t 0)

;; код ниже лежит во flash (@const), а не в куче LispBM
@const-start

;; ======================================================================= 3. помощники

(defun iabs (x) (if (< x 0) (- 0 x) x))
(defun clamp (x lo hi) (if (< x lo) lo (if (> x hi) hi x)))
(defun main-scr (i) (ix (ix mains i) 0))
(defun main-pages (i) (ix (ix mains i) 1))
(defun main-dots (i) (ix (ix mains i) 2))
(defun busy? () (< (secs-since nav-busy-time) nav-busy-for))
(defun set-busy (s) (progn (setq nav-busy-time (systime)) (setq nav-busy-for s)))
(defun other-main () (if (= main-screen 0) 1 0))

;; голый контейнер без стилей темы: ничего лишнего не рисуется
(defun box (par x y w h)
  (let ((o (lv-obj-create par)))
    (progn (lv-obj-remove-style-all o)
           (lv-obj-set-pos o x y)
           (lv-obj-set-size o w h)
           (lv-obj-remove-flag o LV_OBJ_FLAG_CLICKABLE)
           (lv-obj-remove-flag o LV_OBJ_FLAG_SCROLLABLE)
           o)))

;; прокручиваемый контейнер со snap: на нём держится всё "движение за пальцем"
(defun scroller (par w h dir)
  (let ((o (lv-obj-create par)))
    (progn (lv-obj-remove-style-all o)
           (lv-obj-set-size o w h)
           (lv-obj-set-scroll-dir o dir)
           (lv-obj-set-scrollbar-mode o LV_SCROLLBAR_MODE_OFF)
           (lv-obj-add-flag o LV_OBJ_FLAG_SCROLL_ONE)
           (lv-obj-remove-flag o LV_OBJ_FLAG_SCROLL_MOMENTUM)
           o)))

;; ======================================================================= 4. дизайн (только внешний вид)

(def C-BG 0x0B0F14)
(def C-CARD 0x161D27)
(def C-LINE 0x243041)
(def C-TEXT 0xE8EEF5)
(def C-MUTED 0x8696A8)
(def C-BLUE 0x2F80ED)
(def C-GREEN 0x27AE60)
(def C-ORANGE 0xF2994A)
(def C-RED 0xEB5757)
(def C-PURPLE 0x9B51E0)

(defun panel (par x y w h col)
  (let ((o (box par x y w h)))
    (progn (lv-obj-set-style-bg-color o col 0)
           (lv-obj-set-style-bg-opa o 255 0)
           (lv-obj-set-style-radius o 14 0)
           o)))

(defun text (par s font col x y)
  (let ((l (lv-label-create par)))
    (progn (lv-label-set-text l s)
           (lv-obj-set-style-text-font l font 0)
           (lv-obj-set-style-text-color l col 0)
           (lv-obj-set-pos l x y)
           (lv-obj-remove-flag l LV_OBJ_FLAG_CLICKABLE)
           l)))

(defun card (par x y w h title value unit col)
  (let ((c (panel par x y w h C-CARD)))
    (progn (text c title font-montserrat-14 C-MUTED 16 12)
           (text c value font-montserrat-32 col 16 38)
           (text c unit font-montserrat-16 C-MUTED 16 80)
           c)))

(defun bar (par x y w h v col)
  (let ((b (lv-bar-create par)))
    (progn (lv-obj-remove-style-all b)
           (lv-obj-set-pos b x y)
           (lv-obj-set-size b w h)
           (lv-bar-set-range b 0 100)
           (lv-bar-set-value b v 0)
           (lv-obj-set-style-bg-color b C-LINE 0)
           (lv-obj-set-style-bg-opa b 255 0)
           (lv-obj-set-style-radius b (/ h 2) 0)
           (lv-obj-set-style-bg-color b col LV_PART_INDICATOR)
           (lv-obj-set-style-bg-opa b 255 LV_PART_INDICATOR)
           (lv-obj-set-style-radius b (/ h 2) LV_PART_INDICATOR)
           (lv-obj-remove-flag b LV_OBJ_FLAG_CLICKABLE)
           b)))

(defun ring (par x y d w v col)
  (let ((a (lv-arc-create par)))
    (progn (lv-obj-remove-style-all a)
           (lv-obj-set-pos a x y)
           (lv-obj-set-size a d d)
           (lv-arc-set-rotation a 135)
           (lv-arc-set-bg-angles a 0 270)
           (lv-arc-set-range a 0 100)
           (lv-arc-set-value a v)
           (lv-obj-set-style-arc-width a w 0)
           (lv-obj-set-style-arc-color a C-LINE 0)
           (lv-obj-set-style-arc-rounded a 1 0)
           (lv-obj-set-style-arc-width a w LV_PART_INDICATOR)
           (lv-obj-set-style-arc-color a col LV_PART_INDICATOR)
           (lv-obj-set-style-arc-rounded a 1 LV_PART_INDICATOR)
           (lv-obj-remove-flag a LV_OBJ_FLAG_CLICKABLE)
           a)))

(defun chart (par x y w h type vals col)
  (let ((c (lv-chart-create par)))
    (progn (lv-obj-remove-style-all c)
           (lv-obj-set-pos c x y)
           (lv-obj-set-size c w h)
           (lv-obj-set-style-bg-color c C-CARD 0)
           (lv-obj-set-style-bg-opa c 255 0)
           (lv-obj-set-style-radius c 12 0)
           (lv-obj-set-style-pad-left c 12 0) (lv-obj-set-style-pad-right c 12 0)
           (lv-obj-set-style-pad-top c 14 0) (lv-obj-set-style-pad-bottom c 14 0)
           (lv-obj-set-style-line-color c C-LINE 0)
           (lv-obj-set-style-line-width c 1 0)
           (lv-chart-set-type c type)
           (lv-chart-set-div-line-count c 4 0)
           (lv-chart-set-point-count c (length vals))
           (lv-chart-set-axis-range c LV_CHART_AXIS_PRIMARY_Y 0 100)
           (lv-obj-set-style-line-width c 3 LV_PART_ITEMS)
           (lv-obj-set-style-width c 0 LV_PART_INDICATOR)
           (lv-obj-set-style-height c 0 LV_PART_INDICATOR)
           (lv-obj-set-style-pad-column c 6 0)
           (let ((s (lv-chart-add-series c col LV_CHART_AXIS_PRIMARY_Y)))
             (lv-chart-set-series-values c s vals))
           (lv-obj-remove-flag c LV_OBJ_FLAG_CLICKABLE)
           (lv-obj-remove-flag c LV_OBJ_FLAG_SCROLLABLE)
           c)))

(defun led (par x y col on)
  (let ((l (lv-led-create par)))
    (progn (lv-obj-set-pos l x y)
           (lv-obj-set-size l 14 14)
           (lv-led-set-color l col)
           (if on (lv-led-on l) (lv-led-off l))
           (lv-obj-remove-flag l LV_OBJ_FLAG_CLICKABLE)
           l)))

;; кнопка-плитка: включение/выключение делает сам LVGL (флаг CHECKABLE + стиль для CHECKED)
(defun tile (par x y w h label col checkable)
  (let ((b (lv-obj-create par)))
    (progn (lv-obj-remove-style-all b)
           (lv-obj-set-pos b x y)
           (lv-obj-set-size b w h)
           (lv-obj-set-style-bg-color b C-LINE 0)
           (lv-obj-set-style-bg-opa b 255 0)
           (lv-obj-set-style-radius b 14 0)
           (lv-obj-set-style-bg-color b col LV_STATE_CHECKED)
           (lv-obj-set-style-bg-color b 0x34455C LV_STATE_PRESSED)
           (lv-obj-remove-flag b LV_OBJ_FLAG_SCROLLABLE)
           (if checkable (lv-obj-add-flag b LV_OBJ_FLAG_CHECKABLE) nil)
           (let ((l (text b label font-montserrat-16 C-TEXT 0 0)))
             (lv-obj-align l LV_ALIGN_CENTER 0 0))
           b)))

(defun page-title (par s)
  (text par s font-montserrat-20 C-MUTED 28 20))

;; ---- Main Screen 1

(defun m1-p1 (p)
  (progn
    (page-title p "MAIN 1  /  PAGE 1   SPEED")
    (setq speed-ring (ring p 40 70 340 26 40 C-BLUE))
    (setq speed-label (text p "24" font-montserrat-48 C-TEXT 0 0))
    (lv-obj-align-to speed-label speed-ring LV_ALIGN_CENTER 0 -10)
    (let ((u (text p "km/h" font-montserrat-20 C-MUTED 0 0)))
      (lv-obj-align-to u speed-ring LV_ALIGN_CENTER 0 34))
    (card p 420 70 170 120 "BATTERY" "78 %" "51.2 V" C-GREEN)
    (card p 606 70 170 120 "RANGE" "41" "km" C-TEXT)
    (card p 420 206 356 120 "TRIP" "12.6" "km   0:38 h" C-TEXT)
    (bar p 420 350 356 14 78 C-GREEN)
    (text p "ODO 3412 km" font-montserrat-16 C-MUTED 420 378)))

(defun m1-p2 (p)
  (progn
    (page-title p "MAIN 1  /  PAGE 2   CHARTS")
    (text p "POWER, kW" font-montserrat-14 C-MUTED 28 58)
    (chart p 24 80 370 170 LV_CHART_TYPE_LINE (list 10 25 40 35 60 72 55 80 66 40 30 45) C-ORANGE)
    (text p "CURRENT, A" font-montserrat-14 C-MUTED 410 58)
    (chart p 406 80 370 170 LV_CHART_TYPE_BAR (list 30 50 70 45 85 60 40 20) C-BLUE)
    (text p "TEMPERATURE, C" font-montserrat-14 C-MUTED 28 266)
    (chart p 24 288 752 140 LV_CHART_TYPE_LINE (list 20 22 25 30 34 38 41 44 46 45 43 40 38 36 35 34) C-RED)))

(defun m1-p3 (p)
  (progn
    (page-title p "MAIN 1  /  PAGE 3   TEMPERATURE & POWER")
    (card p 24 64 240 120 "MOTOR" "62 C" "limit 110 C" C-ORANGE)
    (card p 280 64 240 120 "CONTROLLER" "48 C" "limit 85 C" C-TEXT)
    (card p 536 64 240 120 "BATTERY" "31 C" "limit 60 C" C-GREEN)
    (bar p 40 170 208 6 56 C-ORANGE)
    (bar p 296 170 208 6 56 C-BLUE)
    (bar p 552 170 208 6 52 C-GREEN)
    (let ((pw (panel p 24 204 752 220 C-CARD)))
      (progn
        (text pw "POWER" font-montserrat-16 C-MUTED 20 16)
        (text pw "3.48 kW" font-montserrat-48 C-PURPLE 20 46)
        (text pw "peak 7.9 kW   regen 1.2 kW" font-montserrat-16 C-MUTED 20 112)
        (ring pw 520 20 180 20 64 C-PURPLE)
        (bar pw 20 160 440 16 44 C-PURPLE)))))

;; ---- Main Screen 2

(defun setting-row (p y label on)
  (progn
    (panel p 24 y 752 64 C-CARD)
    (text p label font-montserrat-20 C-TEXT 48 (+ y 20))
    (let ((s (lv-switch-create p)))
      (progn (lv-obj-set-pos s 680 (+ y 16))
             (lv-obj-set-size s 64 32)
             (if on (lv-obj-add-state s LV_STATE_CHECKED) nil)))))

(defun m2-p1 (p)
  (progn
    (page-title p "MAIN 2  /  PAGE 1   SETTINGS")
    (setting-row p 64 "Units: km/h" t)
    (setting-row p 138 "Auto brightness" nil)
    (setting-row p 212 "Sounds" t)
    (panel p 24 286 752 120 C-CARD)
    (text p "Max speed" font-montserrat-20 C-TEXT 48 302)
    (setq max-speed-label (text p "45 km/h" font-montserrat-20 C-BLUE 640 302))
    (let ((s (lv-slider-create p)))
      (progn (lv-obj-set-pos s 56 354)
             (lv-obj-set-size s 680 14)
             (lv-slider-set-range s 10 90)
             (lv-slider-set-value s 45 0)
             (lv-obj-add-event-cb s 'on-max-speed LV_EVENT_VALUE_CHANGED)))))

(defun m2-p2 (p)
  (progn
    (page-title p "MAIN 2  /  PAGE 2   STATISTICS")
    (card p 24 64 176 120 "TRIPS" "128" "total" C-TEXT)
    (card p 216 64 176 120 "DISTANCE" "3412" "km" C-BLUE)
    (card p 408 64 176 120 "AVG SPEED" "27.4" "km/h" C-GREEN)
    (card p 600 64 176 120 "ENERGY" "19.6" "Wh/km" C-ORANGE)
    (text p "DISTANCE BY DAY, km" font-montserrat-14 C-MUTED 28 200)
    (chart p 24 222 752 206 LV_CHART_TYPE_BAR (list 32 18 44 60 25 70 52) C-GREEN)))

(defun diag-row (p y name state col on)
  (progn
    (led p 44 (+ y 6) col on)
    (text p name font-montserrat-20 C-TEXT 76 y)
    (text p state font-montserrat-20 col 600 y)))

(defun m2-p3 (p)
  (progn
    (page-title p "MAIN 2  /  PAGE 3   DIAGNOSTICS")
    (panel p 24 60 752 300 C-CARD)
    (diag-row p 80 "CAN bus" "OK" C-GREEN t)
    (diag-row p 120 "BMS link" "OK" C-GREEN t)
    (diag-row p 160 "Motor hall sensors" "WARN" C-ORANGE t)
    (diag-row p 200 "Throttle input" "OK" C-GREEN t)
    (diag-row p 240 "SD card" "FAIL" C-RED t)
    (diag-row p 280 "Firmware" "6.05" C-MUTED nil)
    (text p "UPTIME" font-montserrat-14 C-MUTED 28 378)
    (setq uptime-label (text p "0 s" font-montserrat-32 C-TEXT 28 398))))

;; миниатюры для списка Main Screens (рисуются фигурами: снимков экрана нет)
(defun thumb-m1 (t1)
  (progn (ring t1 10 22 110 10 40 C-BLUE)
         (panel t1 136 22 80 44 C-LINE)
         (panel t1 136 76 80 44 C-LINE)
         (bar t1 136 132 80 6 78 C-GREEN)))

(defun thumb-m2 (t1)
  (progn (panel t1 10 22 210 26 C-LINE)
         (panel t1 10 56 210 26 C-LINE)
         (panel t1 10 90 210 26 C-LINE)
         (bar t1 20 128 190 6 45 C-BLUE)))

;; таблица дизайна -- единственное, что навигация знает о содержимом
(def DESIGN (list (list "Main Screen 1" (list m1-p1 m1-p2 m1-p3) thumb-m1)
                  (list "Main Screen 2" (list m2-p1 m2-p2 m2-p3) thumb-m2)))

;; ======================================================================= 5. постройка

;; Main Screen = отдельный экран LVGL; страницы лежат в горизонтальном контейнере со snap
(defun build-dot (scr k)
  (let ((d (box scr (+ (- (/ SW 2) 42) (* k 30)) (- SH 36) 24 8)))
    (progn (lv-obj-set-style-radius d 4 0)
           (lv-obj-set-style-bg-color d C-LINE 0)
           (lv-obj-set-style-bg-opa d 255 0)
           d)))

(defun build-page (pages i k)
  (let ((p (lv-obj-create pages)))
    (progn (lv-obj-remove-style-all p)
           (lv-obj-set-pos p (* k SW) 0)
           (lv-obj-set-size p SW SH)
           (lv-obj-remove-flag p LV_OBJ_FLAG_SCROLLABLE)
           ((ix (ix (ix DESIGN i) 1) k) p)
           p)))

(defun build-main (i)
  (let ((scr (lv-obj-create nil)))
    (progn
      (lv-obj-remove-style-all scr)
      (lv-obj-set-style-bg-color scr C-BG 0)
      (lv-obj-set-style-bg-opa scr 255 0)
      (let ((pages (scroller scr SW SH LV_DIR_HOR)))
        (progn
          (lv-obj-set-scroll-snap-x pages LV_SCROLL_SNAP_CENTER)
          (build-page pages i 0) (build-page pages i 1) (build-page pages i 2)
          (lv-obj-add-event-cb pages 'on-pages-end LV_EVENT_SCROLL_END i)
          (list scr pages (list (build-dot scr 0) (build-dot scr 1) (build-dot scr 2))))))))

;; шторка: вертикальный контейнер на верхнем слое. Содержимое: панель (0..DRAWER-H), язычок
;; (DRAWER-H..+HANDLE-H) и прозрачная прокладка. Прокрутка DRAWER-H = закрыта (виден только язычок
;; у верхнего края), 0 = открыта. Сам контейнер не кликабелен: касания мимо панели и язычка
;; проходят к экрану под ним.

(defun build-drawer ()
  (progn
    (setq drawer (scroller (lv-layer-top) SW SH LV_DIR_VER))
    (lv-obj-remove-flag drawer LV_OBJ_FLAG_CLICKABLE)
    (lv-obj-remove-flag drawer LV_OBJ_FLAG_SCROLL_ELASTIC)
    (lv-obj-set-scroll-snap-y drawer LV_SCROLL_SNAP_START)
    (let ((pn (lv-obj-create drawer)))
      (progn
        (lv-obj-remove-style-all pn)
        (lv-obj-set-pos pn 0 0)
        (lv-obj-set-size pn SW DRAWER-H)
        (lv-obj-remove-flag pn LV_OBJ_FLAG_SCROLLABLE)
        ;; непрозрачная панель без скруглений: LVGL не рисует то, что под ней (проверка перекрытия)
        (lv-obj-set-style-bg-color pn 0x111821 0)
        (lv-obj-set-style-bg-opa pn 255 0)
        (lv-obj-set-style-border-color pn 0x2A3646 0)
        (lv-obj-set-style-border-width pn 1 0)
        (lv-obj-set-style-border-side pn LV_BORDER_SIDE_BOTTOM 0)
        (text pn "CONTROL CENTER" font-montserrat-16 C-MUTED 28 22)
        (tile pn 28 54 170 70 "Bluetooth" C-BLUE t)
        (tile pn 214 54 170 70 "Wi-Fi" C-BLUE t)
        (tile pn 400 54 170 70 "Lights" C-ORANGE t)
        (tile pn 586 54 186 70 "Lock" C-RED t)
        (text pn "BRIGHTNESS" font-montserrat-14 C-MUTED 28 142)
        (let ((s (lv-slider-create pn)))
          (progn (lv-obj-set-pos s 150 144) (lv-obj-set-size s 620 14)
                 (lv-slider-set-range s 5 100) (lv-slider-set-value s 100 0)
                 (lv-obj-add-event-cb s 'on-brightness LV_EVENT_VALUE_CHANGED)))
        (setq drawer-mode-btns
              (list (tile pn 28 180 140 54 "ECO" C-GREEN nil)
                    (tile pn 180 180 140 54 "SPORT" C-ORANGE nil)
                    (tile pn 332 180 140 54 "TURBO" C-RED nil)))
        (lv-obj-add-event-cb (ix drawer-mode-btns 0) 'on-mode LV_EVENT_CLICKED 0)
        (lv-obj-add-event-cb (ix drawer-mode-btns 1) 'on-mode LV_EVENT_CLICKED 1)
        (lv-obj-add-event-cb (ix drawer-mode-btns 2) 'on-mode LV_EVENT_CLICKED 2)
        (lv-obj-add-state (ix drawer-mode-btns 0) LV_STATE_CHECKED)
        (let ((ta (tile pn 488 180 136 54 "Test A" C-PURPLE nil))
              (tb (tile pn 636 180 136 54 "Test B" C-PURPLE nil)))
          (progn (lv-obj-add-event-cb ta 'on-test LV_EVENT_CLICKED 'a)
                 (lv-obj-add-event-cb tb 'on-test LV_EVENT_CLICKED 'b)))
        (text pn "BAT 78 %   51.2 V      TEMP 41 C" font-montserrat-16 C-TEXT 28 252)
        (text pn "fps: top right corner" font-montserrat-16 C-MUTED 470 252)))
    (let ((h (lv-obj-create drawer)))
      (progn
        (lv-obj-remove-style-all h)
        (lv-obj-set-pos h 0 DRAWER-H)
        (lv-obj-set-size h SW HANDLE-H)
        (lv-obj-remove-flag h LV_OBJ_FLAG_SCROLLABLE)
        (let ((pill (box h (- (/ SW 2) 40) 10 80 6)))
          (progn (lv-obj-set-style-radius pill 3 0)
                 (lv-obj-set-style-bg-color pill 0x5A6B80 0)
                 (lv-obj-set-style-bg-opa pill 255 0)))
        (lv-obj-add-event-cb h 'on-drawer-handle LV_EVENT_CLICKED)))
    (setq drawer-spacer (box drawer 0 (+ DRAWER-H HANDLE-H) SW (- SH HANDLE-H)))
    (lv-obj-remove-flag drawer-spacer LV_OBJ_FLAG_SNAPPABLE)
    (lv-obj-set-style-bg-color drawer-spacer 0x000000 0)
    (lv-obj-add-event-cb drawer-spacer 'on-drawer-spacer LV_EVENT_CLICKED)
    (lv-obj-add-event-cb drawer 'on-drawer-end LV_EVENT_SCROLL_END)
    (lv-obj-update-layout drawer)
    (lv-obj-scroll-to-y drawer DRAWER-H LV_ANIM_OFF)))

;; список Main Screens: вертикальный контейнер на верхнем слое: прозрачная прокладка (прокрутка 0 =
;; закрыт) и панель (прокрутка SH = открыт). Карточки -- в горизонтальном контейнере со snap.
(def CARD-W 300)
(def CARD-H 240)
(def CARD-GAP 40)

(defun build-card (row k)
  (let ((c (lv-obj-create row)))
    (progn
      (lv-obj-remove-style-all c)
      (lv-obj-set-pos c (* k (+ CARD-W CARD-GAP)) 0)
      (lv-obj-set-size c CARD-W CARD-H)
      (lv-obj-remove-flag c LV_OBJ_FLAG_SCROLLABLE)
      (lv-obj-set-style-bg-color c C-CARD 0)
      (lv-obj-set-style-bg-opa c 255 0)
      (lv-obj-set-style-radius c 22 0)
      (lv-obj-set-style-border-color c C-BLUE 0)
      (lv-obj-set-style-border-width c 0 0)
      (lv-obj-set-style-bg-color c 0x202A38 LV_STATE_PRESSED)
      (text c (ix (ix DESIGN k) 0) font-montserrat-20 C-TEXT 20 16)
      (let ((t1 (box c 34 50 232 160)))
        (progn (lv-obj-set-style-bg-color t1 C-BG 0)
               (lv-obj-set-style-bg-opa t1 255 0)
               (lv-obj-set-style-radius t1 12 0)
               ((ix (ix DESIGN k) 2) t1)))
      (lv-obj-add-event-cb c 'on-card LV_EVENT_CLICKED k)
      (cons c k))))

(defun build-switcher ()
  (progn
    (setq switcher (scroller (lv-layer-top) SW SH LV_DIR_VER))
    (lv-obj-remove-flag switcher LV_OBJ_FLAG_CLICKABLE)
    (lv-obj-remove-flag switcher LV_OBJ_FLAG_SCROLL_ELASTIC)
    (lv-obj-set-scroll-snap-y switcher LV_SCROLL_SNAP_START)
    (box switcher 0 0 SW SH)                       ; прокладка: точка привязки "закрыт"
    (let ((pn (lv-obj-create switcher)))
      (progn
        (lv-obj-remove-style-all pn)
        (lv-obj-set-pos pn 0 SH)
        (lv-obj-set-size pn SW SH)
        (lv-obj-remove-flag pn LV_OBJ_FLAG_SCROLLABLE)
        (lv-obj-set-style-bg-color pn 0x05080C 0)
        (lv-obj-set-style-bg-opa pn 255 0)
        (text pn "MAIN SCREENS" font-montserrat-20 C-MUTED 28 26)
        (text pn "tap a card to open, swipe down to close" font-montserrat-14 C-MUTED 28 56)
        (setq switcher-row (scroller pn SW (+ CARD-H 20) LV_DIR_HOR))
        (lv-obj-set-pos switcher-row 0 120)
        (lv-obj-set-scroll-snap-x switcher-row LV_SCROLL_SNAP_CENTER)
        (lv-obj-set-style-pad-left switcher-row (/ (- SW CARD-W) 2) 0)
        (lv-obj-set-style-pad-right switcher-row (/ (- SW CARD-W) 2) 0)
        (setq switcher-cards (list (build-card switcher-row 0) (build-card switcher-row 1)))))
    (lv-obj-add-event-cb switcher 'on-switcher-end LV_EVENT_SCROLL_END)
    (lv-obj-add-flag switcher LV_OBJ_FLAG_HIDDEN)))

(defun build-home-pill ()
  (progn (setq home-pill (box (lv-layer-top) (- (/ SW 2) 60) (- SH 14) 120 6))
         (lv-obj-set-style-radius home-pill 3 0)
         (lv-obj-set-style-bg-color home-pill 0x7F8C9D 0)
         (lv-obj-set-style-bg-opa home-pill 200 0)))

;; ======================================================================= 6. навигация

(defun dots-update (i k)
  (let ((ds (main-dots i)))
    (progn (lv-obj-set-style-bg-color (ix ds 0) (if (= k 0) C-TEXT C-LINE) 0)
           (lv-obj-set-style-bg-color (ix ds 1) (if (= k 1) C-TEXT C-LINE) 0)
           (lv-obj-set-style-bg-color (ix ds 2) (if (= k 2) C-TEXT C-LINE) 0))))

;; страница меняется только пальцем (LVGL); здесь лишь запоминается результат
(defun on-pages-end (e)
  (let ((i (lv-event-get-user-data e)))
    (let ((k (clamp (to-i (/ (+ (lv-obj-get-scroll-x (main-pages i)) (/ SW 2)) SW)) 0 (- PAGE-COUNT 1))))
      (progn (setq page-of (if (= i 0) (list k (ix page-of 1)) (list (ix page-of 0) k)))
             (if (= i main-screen) (setq page k) nil)
             (dots-update i k)))))

(defun drawer-sync ()
  (if (ix MAIN-HAS-DRAWER main-screen)
      (lv-obj-remove-flag drawer LV_OBJ_FLAG_HIDDEN)
      (lv-obj-add-flag drawer LV_OBJ_FLAG_HIDDEN)))

(defun go-main (i anim)
  (if (and (not (= i main-screen)) (>= i 0) (< i MAIN-COUNT))
      (progn
        (setq main-screen i)
        (setq page (ix page-of i))
        (drawer-sync)
        (if (= anim LV_SCREEN_LOAD_ANIM_NONE)
            (lv-screen-load (main-scr i))
            (progn (lv-screen-load-anim (main-scr i) anim ANIM-MS 0 nil)
                   (set-busy (/ (+ ANIM-MS 60) 1000.0))))
        (print "[nav] main" (+ i 1) "page" (+ page 1)))
      nil))

;; шторка
(defun drawer-set (open anim) (lv-obj-scroll-to-y drawer (if open 0 DRAWER-H) (if anim LV_ANIM_ON LV_ANIM_OFF)))

(defun on-drawer-end (e)
  (let ((y (lv-obj-get-scroll-y drawer)))
    (cond ((<= y 2) (drawer-state t))
          ((>= y (- DRAWER-H 2)) (drawer-state nil))
          (t nil))))

(defun drawer-state (o)
  (if (eq o drawer-open) nil
      (progn
        (setq drawer-open o)
        ;; открыта: прокладка под панелью ловит касания (тап или свайп вверх закрывают шторку)
        (if o (progn (lv-obj-add-flag drawer-spacer LV_OBJ_FLAG_CLICKABLE)
                     (lv-obj-set-style-bg-opa drawer-spacer 90 0))
              (progn (lv-obj-remove-flag drawer-spacer LV_OBJ_FLAG_CLICKABLE)
                     (lv-obj-set-style-bg-opa drawer-spacer 0 0)))
        (print "[nav] drawer" (if o "open" "closed")))))

(defun on-drawer-spacer (e) (drawer-set nil t))
(defun on-drawer-handle (e) (drawer-set (not drawer-open) t))

;; список Main Screens
(defun switcher-open ()
  (if app-switcher-open nil
      (progn
        (setq app-switcher-open t)
        (setq switcher-open-time (systime))
        (map (lambda (c) (lv-obj-set-style-border-width (car c) (if (= (cdr c) main-screen) 3 0) 0)) switcher-cards)
        (lv-obj-remove-flag switcher LV_OBJ_FLAG_HIDDEN)
        (lv-obj-update-layout switcher)
        (lv-obj-scroll-to-x switcher-row (* main-screen (+ CARD-W CARD-GAP)) LV_ANIM_OFF)
        (lv-obj-scroll-to-y switcher 0 LV_ANIM_OFF)
        (lv-obj-scroll-to-y switcher SH LV_ANIM_ON)
        (print "[nav] switcher open"))))

(defun switcher-close () (lv-obj-scroll-to-y switcher 0 LV_ANIM_ON))

(defun on-switcher-end (e)
  (if (and app-switcher-open (> (secs-since switcher-open-time) 0.2) (<= (lv-obj-get-scroll-y switcher) 2))
      (progn (lv-obj-add-flag switcher LV_OBJ_FLAG_HIDDEN)
             (setq app-switcher-open nil)
             (print "[nav] switcher closed"))
      nil))

;; тап по карточке: экран меняется сразу (он закрыт непрозрачной панелью), панель уезжает вниз
(defun on-card (e)
  (progn (go-main (lv-event-get-user-data e) LV_SCREEN_LOAD_ANIM_NONE)
         (switcher-close)))

;; ======================================================================= 7. жесты (один обработчик)

(defun zone-of (x y)
  (cond ((or drawer-open app-switcher-open (busy?)) 'overlay)
        ((and (< y EDGE-TOP) (ix MAIN-HAS-DRAWER main-screen)) 'top)
        ((> y (- SH EDGE-BOT)) 'bottom)
        (t 'body)))

(defun on-touch (e)
  (let ((x (lv-event-get-x e)) (y (lv-event-get-y e)))
    (if (= (lv-event-get-code e) LV_EVENT_PRESSED) (gesture-begin x y) (gesture-end x y))))

(defun gesture-begin (x y)
  (progn
    (setq gesture-active t)
    (setq gesture-zone (zone-of x y))
    (setq gesture-start-x x) (setq gesture-start-y y)
    (setq gesture-current-x x) (setq gesture-current-y y)
    (setq gesture-still-x x) (setq gesture-still-y y)
    (setq gesture-start-time (systime))
    (setq gesture-still-time gesture-start-time)
    (setq gesture-hold nil)))

(defun gesture-end (x y)
  (if gesture-active
      (let ((dx (- x gesture-start-x)) (dy (- y gesture-start-y)))
        (progn
          (setq gesture-active nil)
          (setq gesture-current-x x) (setq gesture-current-y y)
          (pill-feedback 0)
          (if gesture-hold nil
              (cond
                ;; снизу вверх коротко -> другой Main Screen (уезжает вверх)
                ((eq gesture-zone 'bottom)
                 (if (and (< dy (- 0 SWIPE-MIN)) (> (iabs dy) (iabs dx)))
                     (go-main (other-main) LV_SCREEN_LOAD_ANIM_MOVE_TOP) nil))
                ;; вниз по странице -> другой Main Screen (въезжает сверху)
                ((eq gesture-zone 'body)
                 (if (and (> dy SWIPE-MIN) (> (iabs dy) (* 2 (iabs dx))))
                     (go-main (other-main) LV_SCREEN_LOAD_ANIM_MOVE_BOTTOM) nil))
                ;; top: шторку ведёт LVGL; overlay: шторка или список открыты
                (t nil)))))
      nil))

;; Пока идёт жест снизу: положение пальца опрашивается раз в тик (палец вниз уже не отслеживается
;; событиями объекта). Подъём больше HOLD-UP и неподвижность HOLD-TIME -> удержание -> список.
(defun gesture-tick ()
  (if (and gesture-active (eq gesture-zone 'bottom) (not gesture-hold))
      (let ((tg (lv-touch-get)))
        (if (and tg (= (ix tg 0) 1))
            (let ((x (ix tg 1)) (y (ix tg 2)))
              (progn
                (setq gesture-current-x x) (setq gesture-current-y y)
                (pill-feedback (- gesture-start-y y))
                (if (or (> (iabs (- x gesture-still-x)) HOLD-STILL) (> (iabs (- y gesture-still-y)) HOLD-STILL))
                    (progn (setq gesture-still-x x) (setq gesture-still-y y) (setq gesture-still-time (systime)))
                    (if (and (> (- gesture-start-y y) HOLD-UP) (> (secs-since gesture-still-time) HOLD-TIME))
                        (progn (setq gesture-hold t) (pill-feedback 0) (switcher-open))
                        nil))))
            ;; палец уже поднят, а RELEASED не пришёл (нажатый объект удалили): завершаем жест здесь
            (if tg (gesture-end (ix tg 1) (ix tg 2)) (setq gesture-active nil))))
      nil))

;; полоска внизу поднимается вслед за пальцем (маленький объект -- маленькая перерисовка)
(defun pill-feedback (up)
  (let ((u (clamp (/ up 3) 0 40)))
    (if (= u pill-up) nil
        (progn (setq pill-up u)
               (lv-obj-set-y home-pill (- SH 14 u))))))

;; ======================================================================= 8. содержимое в работе

(defun on-brightness (e) (lv-backlight (lv-event-get-value e)))
(defun on-mode (e)
  (let ((k (lv-event-get-user-data e)))
    (progn (map (lambda (i) (if (= i k) (lv-obj-add-state (ix drawer-mode-btns i) LV_STATE_CHECKED)
                                (lv-obj-remove-state (ix drawer-mode-btns i) LV_STATE_CHECKED)))
                (list 0 1 2))
           (print "[nav] mode" k))))
(defun on-test (e) (print "[nav] test button" (lv-event-get-user-data e)))
(defun on-max-speed (e)
  (lv-label-set-text max-speed-label (str-merge (str-from-n (lv-event-get-value e)) " km/h")))

;; живые значения обновляются только на видимой странице и без открытых панелей
(defun live-tick ()
  (progn
    (if (> (secs-since live-t) 0.2)
        (progn
          (setq live-t (systime))
          (if (and (= main-screen 0) (= page 0) (not drawer-open) (not app-switcher-open) (not (busy?)))
              (let ((v (to-i (+ 30 (* 22 (sin speed-v))))))
                (progn (setq speed-v (+ speed-v 0.12))
                       (lv-arc-set-value speed-ring (/ (* v 100) 60))
                       (lv-label-set-text speed-label (str-from-n v))))
              nil))
        nil)
    (if (> (secs-since slow-t) 1.0)
        (progn
          (setq slow-t (systime))
          (if (and (= main-screen 1) (= page 2) (not app-switcher-open))
              (lv-label-set-text uptime-label (str-merge (str-from-n (to-i (secs-since uptime-t0))) " s"))
              nil)
          (perf-tick))
        nil)))

;; Счётчик кадров: lv-perf считает только реально отрисованные кадры (дольше 1 мс). В покое LVGL
;; ничего не рисует, и кадров почти нет -- это норма, поэтому показывается последнее измерение
;; при движении (листание, шторка, переходы): кадров в секунду, среднее/максимальное время кадра.
;; То же печатается в консоль VESC Tool.
(defun ms1 (us) (str-merge (str-from-n (/ us 1000)) "." (str-from-n (/ (mod us 1000) 100))))
(defun perf-tick ()
  (let ((pf (lv-perf)) (dt (secs-since perf-t)))
    (progn
      (setq perf-t (systime))
      (if (and (>= (ix pf 0) 3) (> dt 0.5))
          (let ((fps (to-i (/ (ix pf 0) dt))))
            (progn
              (lv-label-set-text perf-label
                (str-merge (str-from-n fps) " fps  " (ms1 (ix pf 1)) "/" (ms1 (ix pf 2)) " ms"))
              (print "[perf] fps" fps "frame avg" (ms1 (ix pf 1)) "ms max" (ms1 (ix pf 2))
                     "ms | per s: px" (to-i (/ (ix pf 5) dt)) "k rotate" (ms1 (to-i (/ (ix pf 3) dt)))
                     "ms wait" (ms1 (to-i (/ (ix pf 4) dt))) "ms flush" (ms1 (to-i (/ (ix pf 10) dt)))
                     "ms | int-ram" (ix pf 8) "KB")))
          nil))))

(defun build-perf ()
  (progn (setq perf-label (text (lv-layer-sys) "fps: move something" font-montserrat-14 0xA9BCD0 (- SW 230) 6))
         (lv-obj-set-style-bg-color perf-label 0x000000 0)
         (lv-obj-set-style-bg-opa perf-label 150 0)
         (lv-obj-set-style-pad-left perf-label 6 0)
         (lv-obj-set-style-pad-right perf-label 6 0)))

;; ======================================================================= 9. цикл

(defun dispatch (e) (eval (list (lv-event-get-cb e) (list 'quote e))))
(defun drain () (let ((e (lv-event-poll))) (if e (progn (dispatch e) (drain)) nil)))

(defun main-loop ()
  (progn
    (let ((r (trap (progn (drain) (gesture-tick) (live-tick)))))
      (if (eq (car r) 'exit-error) (progn (print "[nav] error" r) (sleep 0.2)) nil))
    ;; спим до события (просыпаемся сразу), во время жеста снизу -- не дольше 16 мс
    (lv-event-wait (if gesture-active 0.016 0.05))
    (main-loop)))

(defun start ()
  (if (eq (car (trap lv-touch-hook)) 'exit-error)
      (print "[nav] firmware is too old: lv-touch-hook is missing (needs bridge r57+)")
      (progn
        (print "[nav] building...")
        (lv-batch 1)
        (setq mains (list (build-main 0) (build-main 1)))
        (build-home-pill)
        (build-drawer)
        (build-switcher)
        (build-perf)
        (dots-update 0 0) (dots-update 1 0)
        (lv-screen-load (main-scr 0))
        (drawer-sync)
        (lv-batch 0)
        (setq uptime-t0 (systime))
        (setq perf-t (systime))
        (lv-perf)
        (lv-touch-hook 'on-touch)
        (print "[nav] ready: main 1 page 1")
        (main-loop))))

@const-end

(start)
