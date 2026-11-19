// canon-eos-webcam-panel: the settings panel (GTK4). It only talks to the service through the control
// socket: it shows the live preview (zoom / pan to check focus), the six image adjustments and the camera
// settings. Closing it, or it hanging, doesn't affect the webcam.
#include "../common/camera_values.h"
#include "../common/i18n.h"
#include "../common/jpeg.h"
#include "../common/json.h"
#include "../common/util.h"
#include "client.h"

#include <gtk/gtk.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <memory>
#include <vector>

using namespace canon_eos_webcam;

namespace {

struct Prop {
    bool hasValue = false, hasList = false;
    uint32_t value = 0;
    std::vector<uint32_t> list;
    std::vector<uint8_t> raw;
};

std::vector<uint8_t> FromHex(const std::string& h) {
    std::vector<uint8_t> r;
    for (size_t i = 0; i + 1 < h.size(); i += 2) r.push_back(uint8_t(strtoul(h.substr(i, 2).c_str(), nullptr, 16)));
    return r;
}

struct Slider {
    GtkWidget* label = nullptr;
    GtkWidget* scale = nullptr;
    GtkWidget* value = nullptr;
    std::string key;     // key sent with "set"
    bool camera = false; // camera sliders send on release only
    bool pressed = false;
    guint timer = 0;
    int def = 0;
    Slider* self() { return this; }
};

struct Combo {
    GtkWidget* label = nullptr;
    GtkWidget* dd = nullptr;
    GtkWidget* note = nullptr;
    GtkStringList* model = nullptr;
    std::vector<uint32_t> values;
    std::string key;
    Str errLabel = Str::None;
};

class Panel {
public:
    explicit Panel(GtkApplication* app);
    GtkWindow* window() { return GTK_WINDOW(win_); }
    // Offscreen rendering (--snapshot): the widget tree is laid out and drawn to a PNG without showing a window.
    bool Snapshot(const char* path, int w, int h);
    // Feeds a message as if the service had sent it (used by --snapshot).
    void Feed(const Json& j) { OnMessage(j); }
    void FeedState(bool connected) { OnState(connected); }

private:
    // ---- construction
    void BuildUi();
    Slider* AddSlider(GtkGrid* g, int row, Str label, const char* key, int lo, int hi, int def, bool camera);
    Combo* AddCombo(GtkGrid* g, int row, Str label, const char* key, Str errLabel);
    GtkWidget* Group(const char* title, GtkGrid** grid);

    // ---- service messages
    void OnMessage(const Json& j);
    void OnState(bool connected);
    void OnStatus(const Json& j);
    void OnImage(const Json& j);
    void OnCamera(const Json& j);
    void OnPreview(const Json& j);
    void OnResult(const Json& j);
    void SyncCamera();
    void Set(const std::string& key, double value, Str errLabel = Str::None);

    // ---- widgets -> service
    static void OnSliderChanged(GtkRange* r, gpointer d);
    static gboolean OnSliderEvent(GtkEventControllerLegacy* c, GdkEvent* e, gpointer d);
    static gboolean OnSliderTimer(gpointer d);
    static void OnComboChanged(GObject* o, GParamSpec*, gpointer d);
    void SendSlider(Slider* s);
    void UpdateValueLabel(Slider* s);
    void SetSliderValue(Slider* s, int v);
    void EnableSlider(Slider* s, bool on);
    void FillCombo(Combo* c, const std::vector<uint32_t>& values, std::string (*namer)(uint32_t), uint32_t current,
                   bool hasCurrent);
    void ClearCombo(Combo* c);

    // ---- preview
    static void DrawPreview(GtkDrawingArea*, cairo_t* cr, int w, int h, gpointer d);
    void SetZoom(int index);  // 0 fit, 1 = 100%, 2 = 200%, 3 = 300%
    void ClampPan(int w, int h);
    double Scale(int w, int h) const;

    GtkApplication* app_;
    GtkWidget* win_ = nullptr;
    GtkWidget* area_ = nullptr;
    GtkWidget* info_ = nullptr;
    GtkWidget* msg_ = nullptr;
    GtkWidget *st0_ = nullptr, *st1_ = nullptr, *st2_ = nullptr;
    GtkWidget* zoomBtn_[4] = {};
    std::unique_ptr<ServiceClient> client_;
    std::vector<std::unique_ptr<Slider>> sliders_;
    std::vector<std::unique_ptr<Combo>> combos_;
    std::map<std::string, Slider*> sliderByKey_;
    Slider *kelvin_ = nullptr, *psSlider_[4] = {};
    Combo *exp_ = nullptr, *iso_ = nullptr, *wb_ = nullptr, *style_ = nullptr, *alo_ = nullptr;
    GtkWidget* reset_ = nullptr;
    GtkWidget* af_ = nullptr;
    Combo* scaleMode_ = nullptr;
    Slider* scaleSharp_ = nullptr;

    bool updating_ = false;
    bool connected_ = false;
    bool camConnected_ = false;
    std::string camName_;
    std::map<uint32_t, Prop> props_;
    int64_t nextId_ = 1;
    std::map<int64_t, Str> pending_;

    // preview
    cairo_surface_t* frame_ = nullptr;
    int frameW_ = 0, frameH_ = 0;
    int zoom_ = 0;
    double panX_ = 0, panY_ = 0, panStartX_ = 0, panStartY_ = 0;
    bool haveFrame_ = false;
    std::string svcState_;
    double fpsIn_ = 0, fpsOut_ = 0, ms_ = 0;
    int consumers_ = 0;
};

// ---------------------------------------------------------------- construction

GtkWidget* Panel::Group(const char* title, GtkGrid** grid) {
    GtkWidget* frame = gtk_frame_new(title);
    GtkWidget* g = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(g), 6);
    gtk_grid_set_column_spacing(GTK_GRID(g), 10);
    gtk_widget_set_margin_start(g, 10);
    gtk_widget_set_margin_end(g, 10);
    gtk_widget_set_margin_top(g, 8);
    gtk_widget_set_margin_bottom(g, 10);
    gtk_frame_set_child(GTK_FRAME(frame), g);
    *grid = GTK_GRID(g);
    return frame;
}

void Panel::UpdateValueLabel(Slider* s) {
    int v = int(std::lround(gtk_range_get_value(GTK_RANGE(s->scale))));
    char b[40];
    const std::string& k = s->key;
    if (k == "image.gamma") snprintf(b, sizeof b, "%.2f", v / 100.0);
    else if (k == "scale.sharpness") snprintf(b, sizeof b, "%.1f", v / 10.0);
    else if (k == "image.saturation") snprintf(b, sizeof b, "%d%%", v);
    else if (k == "cam.kelvin") snprintf(b, sizeof b, "%d K", v);
    else if (k == "cam.style.sharpness") snprintf(b, sizeof b, "%d", v);
    else if (v == 0) snprintf(b, sizeof b, "0");
    else snprintf(b, sizeof b, "%+d", v);
    gtk_label_set_text(GTK_LABEL(s->value), b);
}

void Panel::SetSliderValue(Slider* s, int v) {
    updating_ = true;
    gtk_range_set_value(GTK_RANGE(s->scale), v);
    UpdateValueLabel(s);
    updating_ = false;
}

void Panel::EnableSlider(Slider* s, bool on) {
    gtk_widget_set_sensitive(s->scale, on);
    gtk_widget_set_sensitive(s->label, on);
    gtk_widget_set_sensitive(s->value, on);
}

Slider* Panel::AddSlider(GtkGrid* g, int row, Str label, const char* key, int lo, int hi, int def, bool camera) {
    auto s = std::make_unique<Slider>();
    s->key = key;
    s->camera = camera;
    s->def = def;
    s->label = gtk_label_new(Tr(label));
    gtk_label_set_xalign(GTK_LABEL(s->label), 0);
    s->scale = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, lo, hi, 1);
    gtk_scale_set_draw_value(GTK_SCALE(s->scale), FALSE);
    gtk_widget_set_hexpand(s->scale, TRUE);
    gtk_widget_set_size_request(s->scale, 180, -1);
    gtk_range_set_value(GTK_RANGE(s->scale), def);
    s->value = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(s->value), 1);
    gtk_widget_set_size_request(s->value, 64, -1);
    gtk_grid_attach(g, s->label, 0, row, 1, 1);
    gtk_grid_attach(g, s->scale, 1, row, 1, 1);
    gtk_grid_attach(g, s->value, 2, row, 1, 1);
    g_signal_connect(s->scale, "value-changed", G_CALLBACK(OnSliderChanged), s.get());
    GtkEventController* leg = gtk_event_controller_legacy_new();
    gtk_event_controller_set_propagation_phase(leg, GTK_PHASE_CAPTURE);
    g_signal_connect(leg, "event", G_CALLBACK(OnSliderEvent), s.get());
    gtk_widget_add_controller(s->scale, leg);
    UpdateValueLabel(s.get());
    Slider* p = s.get();
    sliderByKey_[key] = p;
    sliders_.push_back(std::move(s));
    return p;
}

Combo* Panel::AddCombo(GtkGrid* g, int row, Str label, const char* key, Str errLabel) {
    auto c = std::make_unique<Combo>();
    c->key = key;
    c->errLabel = errLabel;
    c->label = gtk_label_new(Tr(label));
    gtk_label_set_xalign(GTK_LABEL(c->label), 0);
    c->model = gtk_string_list_new(nullptr);
    c->dd = gtk_drop_down_new(G_LIST_MODEL(c->model), nullptr);
    gtk_widget_set_hexpand(c->dd, TRUE);
    c->note = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(c->note), 1);
    gtk_grid_attach(g, c->label, 0, row, 1, 1);
    gtk_grid_attach(g, c->dd, 1, row, 1, 1);
    gtk_grid_attach(g, c->note, 2, row, 1, 1);
    g_signal_connect(c->dd, "notify::selected", G_CALLBACK(OnComboChanged), c.get());
    Combo* p = c.get();
    combos_.push_back(std::move(c));
    return p;
}

void Panel::BuildUi() {
    win_ = app_ ? gtk_application_window_new(app_) : gtk_window_new();
    g_object_set_data(G_OBJECT(win_), "panel", this);  // the widget callbacks find the panel through the window
    gtk_window_set_title(GTK_WINDOW(win_), Tr(Str::PanelTitle));
    gtk_window_set_default_size(GTK_WINDOW(win_), 1280, 800);

    GtkWidget* root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_window_set_child(GTK_WINDOW(win_), root);
    GtkWidget* main = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_widget_set_vexpand(main, TRUE);
    gtk_widget_set_margin_start(main, 12);
    gtk_widget_set_margin_end(main, 12);
    gtk_widget_set_margin_top(main, 12);
    gtk_widget_set_margin_bottom(main, 8);
    gtk_box_append(GTK_BOX(root), main);

    // ---- left: preview
    GtkWidget* left = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_set_hexpand(left, TRUE);
    gtk_box_append(GTK_BOX(main), left);
    area_ = gtk_drawing_area_new();
    gtk_widget_set_hexpand(area_, TRUE);
    gtk_widget_set_vexpand(area_, TRUE);
    gtk_widget_set_size_request(area_, 480, 270);
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(area_), DrawPreview, this, nullptr);
    gtk_box_append(GTK_BOX(left), area_);

    GtkGesture* drag = gtk_gesture_drag_new();
    g_signal_connect(drag, "drag-begin", G_CALLBACK(+[](GtkGestureDrag*, double, double, gpointer d) {
        auto* p = static_cast<Panel*>(d);
        p->panStartX_ = p->panX_;
        p->panStartY_ = p->panY_;
    }), this);
    g_signal_connect(drag, "drag-update", G_CALLBACK(+[](GtkGestureDrag*, double dx, double dy, gpointer d) {
        auto* p = static_cast<Panel*>(d);
        if (p->zoom_ == 0) return;
        p->panX_ = p->panStartX_ + dx;
        p->panY_ = p->panStartY_ + dy;
        p->ClampPan(gtk_widget_get_width(p->area_), gtk_widget_get_height(p->area_));
        gtk_widget_queue_draw(p->area_);
    }), this);
    gtk_widget_add_controller(area_, GTK_EVENT_CONTROLLER(drag));
    GtkGesture* click = gtk_gesture_click_new();
    g_signal_connect(click, "pressed", G_CALLBACK(+[](GtkGestureClick*, int n, double, double, gpointer d) {
        if (n == 2) static_cast<Panel*>(d)->SetZoom(0);  // double-click: fit
    }), this);
    gtk_widget_add_controller(area_, GTK_EVENT_CONTROLLER(click));
    GtkEventController* scroll = gtk_event_controller_scroll_new(GTK_EVENT_CONTROLLER_SCROLL_VERTICAL);
    g_signal_connect(scroll, "scroll", G_CALLBACK(+[](GtkEventControllerScroll*, double, double dy, gpointer d) -> gboolean {
        auto* p = static_cast<Panel*>(d);
        int z = std::max(0, std::min(3, p->zoom_ + (dy < 0 ? 1 : -1)));
        if (z != p->zoom_) p->SetZoom(z);
        return TRUE;
    }), this);
    gtk_widget_add_controller(area_, scroll);

    GtkWidget* zrow = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_box_append(GTK_BOX(zrow), gtk_label_new(Tr(Str::ZoomLabel)));
    const char* names[4] = {Tr(Str::ZoomFit), "100%", "200%", "300%"};
    for (int i = 0; i < 4; i++) {
        zoomBtn_[i] = gtk_toggle_button_new_with_label(names[i]);
        if (i > 0) gtk_toggle_button_set_group(GTK_TOGGLE_BUTTON(zoomBtn_[i]), GTK_TOGGLE_BUTTON(zoomBtn_[0]));
        g_object_set_data(G_OBJECT(zoomBtn_[i]), "zoom", GINT_TO_POINTER(i));
        g_signal_connect(zoomBtn_[i], "toggled", G_CALLBACK(+[](GtkToggleButton* b, gpointer d) {
            if (!gtk_toggle_button_get_active(b)) return;
            static_cast<Panel*>(d)->SetZoom(GPOINTER_TO_INT(g_object_get_data(G_OBJECT(b), "zoom")));
        }), this);
        gtk_box_append(GTK_BOX(zrow), zoomBtn_[i]);
    }
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(zoomBtn_[0]), TRUE);
    GtkWidget* hint = gtk_label_new(Tr(Str::ZoomHint));
    gtk_widget_add_css_class(hint, "dim-label");
    gtk_widget_set_hexpand(hint, TRUE);
    gtk_label_set_xalign(GTK_LABEL(hint), 1);
    gtk_label_set_ellipsize(GTK_LABEL(hint), PANGO_ELLIPSIZE_END);
    gtk_box_append(GTK_BOX(zrow), hint);
    gtk_box_append(GTK_BOX(left), zrow);

    // ---- right: image and camera
    GtkWidget* scrolled = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrolled), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_propagate_natural_height(GTK_SCROLLED_WINDOW(scrolled), TRUE);
    gtk_widget_set_size_request(scrolled, 430, -1);
    gtk_box_append(GTK_BOX(main), scrolled);
    GtkWidget* right = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scrolled), right);

    GtkGrid* g;
    gtk_box_append(GTK_BOX(right), Group(Tr(Str::GroupImage), &g));
    AddSlider(g, 0, Str::ImgBrightness, "image.brightness", -100, 100, 0, false);
    AddSlider(g, 1, Str::ImgContrast, "image.contrast", -100, 100, 0, false);
    AddSlider(g, 2, Str::ImgGamma, "image.gamma", 50, 200, 100, false);
    AddSlider(g, 3, Str::ImgTemperature, "image.temperature", -100, 100, 0, false);
    AddSlider(g, 4, Str::ImgTint, "image.tint", -100, 100, 0, false);
    AddSlider(g, 5, Str::ImgSaturation, "image.saturation", 0, 200, 100, false);
    reset_ = gtk_button_new_with_label(Tr(Str::ImgReset));
    gtk_widget_set_halign(reset_, GTK_ALIGN_START);
    g_signal_connect(reset_, "clicked", G_CALLBACK(+[](GtkButton*, gpointer d) {
        auto* p = static_cast<Panel*>(d);
        for (auto& s : p->sliders_)
            if (s->key.rfind("image.", 0) == 0) {  // the image adjustments only, not the scaling
                p->SetSliderValue(s.get(), s->def);
                p->SendSlider(s.get());
            }
    }), this);
    gtk_grid_attach(g, reset_, 0, 6, 3, 1);

    // scaling of the 1056x594 crop to 1920x1080: lets the user compare FSR 1 and bilinear live
    gtk_box_append(GTK_BOX(right), Group(Tr(Str::GroupScale), &g));
    scaleMode_ = AddCombo(g, 0, Str::ScaleMethod, "scale.mode", Str::None);
    FillCombo(scaleMode_, {0, 1}, +[](uint32_t v) { return std::string(Tr(v ? Str::ScaleBilinear : Str::ScaleFsr)); },
              0, true);
    scaleSharp_ = AddSlider(g, 1, Str::ScaleSharpness, "scale.sharpness", 0, 20, 2, false);
    GtkWidget* scaleHint = gtk_label_new(Tr(Str::ScaleHint));
    gtk_label_set_xalign(GTK_LABEL(scaleHint), 0);
    gtk_label_set_wrap(GTK_LABEL(scaleHint), TRUE);
    gtk_widget_add_css_class(scaleHint, "dim-label");
    gtk_grid_attach(g, scaleHint, 0, 2, 3, 1);

    gtk_box_append(GTK_BOX(right), Group(Tr(Str::GroupCamera), &g));
    info_ = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(info_), 0);
    gtk_label_set_wrap(GTK_LABEL(info_), TRUE);
    gtk_grid_attach(g, info_, 0, 0, 3, 1);
    exp_ = AddCombo(g, 1, Str::CamExpComp, "cam.exp", Str::LabelExpComp);
    iso_ = AddCombo(g, 2, Str::CamIso, "cam.iso", Str::LabelIso);
    wb_ = AddCombo(g, 3, Str::CamWhiteBalance, "cam.wb", Str::LabelWhiteBalance);
    kelvin_ = AddSlider(g, 4, Str::CamColorTemp, "cam.kelvin", 2500, 10000, 5200, true);
    style_ = AddCombo(g, 5, Str::CamPictureStyle, "cam.style", Str::LabelPictureStyle);
    psSlider_[0] = AddSlider(g, 6, Str::CamPsContrast, "cam.style.contrast", -4, 4, 0, true);
    psSlider_[1] = AddSlider(g, 7, Str::CamPsSharpness, "cam.style.sharpness", 0, 7, 0, true);
    psSlider_[2] = AddSlider(g, 8, Str::CamPsSaturation, "cam.style.saturation", -4, 4, 0, true);
    psSlider_[3] = AddSlider(g, 9, Str::CamPsColorTone, "cam.style.tone", -4, 4, 0, true);
    for (int i = 0; i < 4; i++) gtk_widget_set_margin_start(psSlider_[i]->label, 16);
    alo_ = AddCombo(g, 10, Str::CamAlo, "cam.alo", Str::LabelAlo);
    msg_ = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(msg_), 0);
    gtk_label_set_wrap(GTK_LABEL(msg_), TRUE);
    af_ = gtk_button_new_with_label(Tr(Str::CamAutoFocus));
    gtk_widget_set_halign(af_, GTK_ALIGN_START);
    g_signal_connect(af_, "clicked", G_CALLBACK(+[](GtkButton*, gpointer d) {
        static_cast<Panel*>(d)->Set("cam.autofocus", 1, Str::LabelAutoFocus);
    }), this);
    gtk_grid_attach(g, af_, 0, 11, 3, 1);
    gtk_grid_attach(g, msg_, 0, 12, 3, 1);

    // room for the AI effects group (NVIDIA Maxine), which comes later
    GtkWidget* fx = gtk_frame_new(Tr(Str::GroupEffectsSoon));
    gtk_widget_set_sensitive(fx, FALSE);
    gtk_widget_set_size_request(fx, -1, 40);
    gtk_box_append(GTK_BOX(right), fx);

    // ---- status bar
    GtkWidget* bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 24);
    gtk_widget_set_margin_start(bar, 12);
    gtk_widget_set_margin_end(bar, 12);
    gtk_widget_set_margin_bottom(bar, 8);
    st0_ = gtk_label_new("");
    st1_ = gtk_label_new("");
    st2_ = gtk_label_new("");
    for (GtkWidget* l : {st0_, st1_, st2_}) {
        gtk_label_set_xalign(GTK_LABEL(l), 0);
        gtk_widget_add_css_class(l, "dim-label");
        gtk_box_append(GTK_BOX(bar), l);
    }
    gtk_box_append(GTK_BOX(root), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));
    gtk_box_append(GTK_BOX(root), bar);

    for (auto& s : sliders_) UpdateValueLabel(s.get());
    SyncCamera();
    OnState(false);
}

Panel::Panel(GtkApplication* app) : app_(app) {
    BuildUi();
    if (!app_) return;  // --snapshot: no service connection
    client_ = std::make_unique<ServiceClient>([this](const Json& j) { OnMessage(j); },
                                              [this](bool c) { OnState(c); });
    client_->Start();
}

bool Panel::Snapshot(const char* path, int w, int h) {
    GtkWidget* root = gtk_window_get_child(GTK_WINDOW(win_));
    // Realized (so styles and fonts work) but never mapped: no surface is committed, so nothing appears on screen.
    gtk_widget_realize(win_);
    gtk_widget_map(root);
    int minW, natW, minH, natH;
    gtk_widget_measure(root, GTK_ORIENTATION_HORIZONTAL, -1, &minW, &natW, nullptr, nullptr);
    gtk_widget_measure(root, GTK_ORIENTATION_VERTICAL, w, &minH, &natH, nullptr, nullptr);
    gtk_widget_allocate(root, std::max(w, minW), std::max(h, minH), -1, nullptr);
    GdkPaintable* pt = gtk_widget_paintable_new(root);
    GtkSnapshot* snap = gtk_snapshot_new();
    {   // the window paints its own background; reproduce it
        GdkRGBA bg;
        G_GNUC_BEGIN_IGNORE_DEPRECATIONS
        if (!gtk_style_context_lookup_color(gtk_widget_get_style_context(win_), "window_bg_color", &bg))
            bg = {0.2f, 0.2f, 0.2f, 1.0f};
        G_GNUC_END_IGNORE_DEPRECATIONS
        graphene_rect_t full = GRAPHENE_RECT_INIT(0, 0, float(w), float(h));
        gtk_snapshot_append_color(snap, &bg, &full);
    }
    gtk_widget_snapshot_child(win_, root, snap);
    GskRenderNode* node = gtk_snapshot_free_to_node(snap);
    GskRenderer* r = gsk_cairo_renderer_new();
    GError* err = nullptr;
    bool ok = false;
    if (gsk_renderer_realize_for_display(r, gdk_display_get_default(), &err)) {
        graphene_rect_t vp = GRAPHENE_RECT_INIT(0, 0, float(w), float(h));
        GdkTexture* tex = gsk_renderer_render_texture(r, node, &vp);
        ok = tex && gdk_texture_save_to_png(tex, path);
        if (tex) g_object_unref(tex);
        gsk_renderer_unrealize(r);
    } else {
        fprintf(stderr, "snapshot: %s\n", err ? err->message : "renderer");
        g_clear_error(&err);
    }
    g_object_unref(r);
    gsk_render_node_unref(node);
    g_object_unref(pt);
    return ok;
}

// ---------------------------------------------------------------- widgets -> service

void Panel::Set(const std::string& key, double value, Str errLabel) {
    if (!client_ || !client_->Connected()) return;
    Json j = Json::Object();
    j.Set("cmd", "set");
    j.Set("key", key);
    j.Set("value", value);
    int64_t id = nextId_++;
    j.Set("id", id);
    if (errLabel != Str::None) pending_[id] = errLabel;
    client_->Send(j);
}

void Panel::SendSlider(Slider* s) {
    if (s->timer) { g_source_remove(s->timer); s->timer = 0; }
    double v = gtk_range_get_value(GTK_RANGE(s->scale));
    if (s->key == "image.gamma") v /= 100.0;
    else if (s->key == "scale.sharpness") v /= 10.0;
    Set(s->key, std::round(v * 100) / 100, s->camera ? (s->key == "cam.kelvin" ? Str::LabelColorTemp : Str::LabelPsParam) : Str::None);
}

void Panel::OnSliderChanged(GtkRange*, gpointer d) {
    auto* s = static_cast<Slider*>(d);
    // the panel is the only one holding a pointer to its sliders: find it through the application
    auto* self = static_cast<Panel*>(g_object_get_data(G_OBJECT(gtk_widget_get_root(s->scale)), "panel"));
    if (!self) return;
    self->UpdateValueLabel(s);
    if (self->updating_) return;
    if (!s->camera) { self->SendSlider(s); return; }  // image: live while dragging
    if (s->pressed) return;                           // camera: on release (see OnSliderEvent)
    // keyboard / wheel: send shortly after the last change
    if (s->timer) g_source_remove(s->timer);
    s->timer = g_timeout_add(450, OnSliderTimer, s);
}

gboolean Panel::OnSliderTimer(gpointer d) {
    auto* s = static_cast<Slider*>(d);
    s->timer = 0;
    auto* self = static_cast<Panel*>(g_object_get_data(G_OBJECT(gtk_widget_get_root(s->scale)), "panel"));
    if (self) self->SendSlider(s);
    return G_SOURCE_REMOVE;
}

gboolean Panel::OnSliderEvent(GtkEventControllerLegacy*, GdkEvent* e, gpointer d) {
    auto* s = static_cast<Slider*>(d);
    GdkEventType t = gdk_event_get_event_type(e);
    if (t == GDK_BUTTON_PRESS || t == GDK_TOUCH_BEGIN) s->pressed = true;
    if ((t == GDK_BUTTON_RELEASE || t == GDK_TOUCH_END) && s->pressed) {
        s->pressed = false;
        if (s->camera) {
            auto* self = static_cast<Panel*>(g_object_get_data(G_OBJECT(gtk_widget_get_root(s->scale)), "panel"));
            if (self && !self->updating_) self->SendSlider(s);
        }
    }
    return GDK_EVENT_PROPAGATE;
}

void Panel::OnComboChanged(GObject* o, GParamSpec*, gpointer d) {
    auto* c = static_cast<Combo*>(d);
    auto* self = static_cast<Panel*>(g_object_get_data(G_OBJECT(gtk_widget_get_root(GTK_WIDGET(o))), "panel"));
    if (!self || self->updating_) return;
    guint sel = gtk_drop_down_get_selected(GTK_DROP_DOWN(o));
    if (sel == GTK_INVALID_LIST_POSITION || sel >= c->values.size()) return;
    self->Set(c->key, double(int32_t(c->values[sel])), c->errLabel);
}

// ---------------------------------------------------------------- service -> widgets

void Panel::OnState(bool connected) {
    connected_ = connected;
    if (!connected) {
        camConnected_ = false;
        props_.clear();
        svcState_ = "";
        gtk_label_set_text(GTK_LABEL(st0_), (std::string(Tr(Str::StatusService)) + Tr(Str::StatusStopped)).c_str());
        gtk_label_set_text(GTK_LABEL(st1_), "");
        gtk_label_set_text(GTK_LABEL(st2_), "");
        gtk_label_set_text(GTK_LABEL(msg_), Tr(Str::ServiceNotRunning));
        haveFrame_ = false;
        SyncCamera();
        gtk_widget_queue_draw(area_);
    } else {
        gtk_label_set_text(GTK_LABEL(msg_), "");
    }
}

void Panel::OnMessage(const Json& j) {
    const std::string& t = j["type"].Str();
    if (t == "status") OnStatus(j);
    else if (t == "image") OnImage(j);
    else if (t == "camera") OnCamera(j);
    else if (t == "preview") OnPreview(j);
    else if (t == "result") OnResult(j);
}

void Panel::OnStatus(const Json& j) {
    svcState_ = j["state"].Str();
    fpsIn_ = j["fps_in"].Num();
    fpsOut_ = j["fps_out"].Num();
    ms_ = j["ms"].Num();
    consumers_ = j["consumers"].Int();
    bool cam = j["camera"].Bool();
    Str st = svcState_ == "live" ? (fpsIn_ > 0.5 ? Str::StatusLive : Str::StatusStartingLiveView)
           : svcState_ == "starting" ? Str::StatusStartingCamera
           : svcState_ == "nocamera" ? Str::StatusNoCamera
                                     : Str::StatusIdle;
    std::string t0 = std::string(Tr(Str::StatusService)) + Tr(st);
    if (cam && !j["name"].Str().empty()) t0 += "  |  " + j["name"].Str();
    char b[200];
    snprintf(b, sizeof b, Tr(Str::StatusFps), fpsIn_, fpsOut_);
    gtk_label_set_text(GTK_LABEL(st0_), t0.c_str());
    gtk_label_set_text(GTK_LABEL(st1_), (std::string(b)).c_str());
    snprintf(b, sizeof b, Tr(Str::StatusProcessing), ms_);
    std::string t2 = b;
    t2 += std::string("  |  ") + (consumers_ > 0 ? Tr(Str::StatusInUse) : Tr(Str::StatusNotInUse));
    gtk_label_set_text(GTK_LABEL(st2_), t2.c_str());
}

void Panel::OnImage(const Json& j) {
    if (j.Has("upscale")) {
        bool fsr = j["upscale"].Str() != "bilinear";
        updating_ = true;
        gtk_drop_down_set_selected(GTK_DROP_DOWN(scaleMode_->dd), fsr ? 0 : 1);
        updating_ = false;
        EnableSlider(scaleSharp_, fsr);  // the sharpening is part of FSR (RCAS); bilinear has none
    }
    if (j.Has("sharpness") && !scaleSharp_->pressed)
        SetSliderValue(scaleSharp_, int(std::lround(j["sharpness"].Num() * 10)));
    for (auto& s : sliders_) {
        if (s->key.rfind("image.", 0) != 0 || s->pressed) continue;
        const std::string name = s->key.substr(6);
        if (!j.Has(name)) continue;
        double v = j[name].Num();
        SetSliderValue(s.get(), int(std::lround(name == "gamma" ? v * 100 : v)));
    }
}

void Panel::OnCamera(const Json& j) {
    camConnected_ = j["connected"].Bool();
    camName_ = j["name"].Str();
    props_.clear();
    for (auto& kv : j["props"].Members()) {
        uint32_t code = uint32_t(strtoul(kv.first.c_str(), nullptr, 16));
        Prop p;
        const Json& v = kv.second;
        if (v.Has("value")) {
            p.hasValue = true;
            p.value = uint32_t(v["value"].Num());
            p.raw = FromHex(v["raw"].Str());
        }
        if (v.Has("list")) {
            p.hasList = true;
            for (const Json& x : v["list"].Items()) p.list.push_back(uint32_t(x.Num()));
        }
        props_[code] = std::move(p);
    }
    SyncCamera();
}

void Panel::OnResult(const Json& j) {
    int64_t id = int64_t(j["id"].Num());
    auto it = pending_.find(id);
    Str label = it == pending_.end() ? Str::None : it->second;
    if (it != pending_.end()) pending_.erase(it);
    if (j["ok"].Bool()) { gtk_label_set_text(GTK_LABEL(msg_), ""); return; }
    if (label == Str::None) return;
    const std::string& e = j["error"].Str();
    char b[300];
    const char* what = Tr(label);
    uint32_t code = uint32_t(j["code"].Num(0));
    if (label == Str::LabelAutoFocus && code && (e == "refused" || e == "failed")) {
        char c[16];
        snprintf(c, sizeof c, "0x%04X", code);
        snprintf(b, sizeof b, Tr(Str::CamErrAutoFocus), c);
    }
    else if (e == "refused") snprintf(b, sizeof b, Tr(Str::CamErrRefused), what);
    else if (e == "not_connected") snprintf(b, sizeof b, Tr(Str::CamErrNotConnected), what);
    else snprintf(b, sizeof b, Tr(Str::CamErrOther), what);
    gtk_label_set_text(GTK_LABEL(msg_), b);
    SyncCamera();  // puts the controls back at what the camera really has
}

void Panel::ClearCombo(Combo* c) {
    c->values.clear();
    guint n = g_list_model_get_n_items(G_LIST_MODEL(c->model));
    gtk_string_list_splice(c->model, 0, n, nullptr);
}

void Panel::FillCombo(Combo* c, const std::vector<uint32_t>& values, std::string (*namer)(uint32_t), uint32_t current,
                      bool hasCurrent) {
    c->values = values;
    std::vector<std::string> names;
    for (uint32_t v : values) names.push_back(namer(v));
    std::vector<const char*> ptrs;
    for (auto& s : names) ptrs.push_back(s.c_str());
    ptrs.push_back(nullptr);
    guint n = g_list_model_get_n_items(G_LIST_MODEL(c->model));
    gtk_string_list_splice(c->model, 0, n, ptrs.data());
    guint sel = GTK_INVALID_LIST_POSITION;
    for (size_t i = 0; i < values.size(); i++)
        if (hasCurrent && values[i] == current) sel = guint(i);
    gtk_drop_down_set_selected(GTK_DROP_DOWN(c->dd), sel);
}

// Same rules as the Windows panel (src/app/panel.cpp, UpdateCamera): only what the current mode announces is enabled.
void Panel::SyncCamera() {
    updating_ = true;
    bool conn = connected_ && camConnected_;
    char b[300];
    if (!conn) gtk_label_set_text(GTK_LABEL(info_), Tr(Str::CamDisconnected));
    else {
        snprintf(b, sizeof b, Tr(Str::CamModeHint), camName_.c_str());
        gtk_label_set_text(GTK_LABEL(info_), b);
    }
    auto prop = [&](uint32_t code) -> const Prop* {
        auto it = props_.find(code);
        return it == props_.end() ? nullptr : &it->second;
    };
    auto setCombo = [&](Combo* c, bool has, const std::vector<uint32_t>& vals, std::string (*namer)(uint32_t),
                        uint32_t cur, bool hasCur) {
        if (has) FillCombo(c, vals, namer, cur, hasCur); else ClearCombo(c);
        gtk_widget_set_sensitive(c->dd, has);
        gtk_widget_set_sensitive(c->label, has);
        gtk_label_set_text(GTK_LABEL(c->note), "");
    };
    const Prop* p = conn ? prop(kPropExpComp) : nullptr;
    bool has = p && p->hasList && !p->list.empty();
    setCombo(exp_, has, has ? p->list : std::vector<uint32_t>{}, ExpCompText, has ? p->value : 0, has && p->hasValue);
    p = conn ? prop(kPropISO) : nullptr;
    has = p && p->hasList && !p->list.empty();
    setCombo(iso_, has, has ? p->list : std::vector<uint32_t>{}, IsoName, has ? p->value : 0, has && p->hasValue);
    if (conn && !has && p && p->hasValue) gtk_label_set_text(GTK_LABEL(iso_->note), Tr(Str::CamIsoAuto));
    const Prop* wb = conn ? prop(kPropWhiteBalance) : nullptr;
    has = wb && wb->hasList && !wb->list.empty();
    setCombo(wb_, has, has ? wb->list : std::vector<uint32_t>{}, WbName, has ? wb->value : 0, has && wb->hasValue);
    // color temperature: only if the camera offers the "color temperature" mode (value 9) and it is selected
    bool kelvinOk = has && std::find(wb->list.begin(), wb->list.end(), 9u) != wb->list.end();
    const Prop* kt = kelvinOk ? prop(kPropColorTemp) : nullptr;
    if (kt && kt->hasValue) SetSliderValue(kelvin_, int(std::min<uint32_t>(10000, std::max<uint32_t>(2500, kt->value))));
    EnableSlider(kelvin_, kelvinOk && wb->hasValue && wb->value == 9);
    if (!kelvinOk) gtk_label_set_text(GTK_LABEL(kelvin_->value), conn ? Tr(Str::NotAvailable) : "");
    // Picture Style and its parameters
    const Prop* st = conn ? prop(kPropPictureStyle) : nullptr;
    has = st && st->hasList && !st->list.empty();
    std::vector<uint32_t> styles;
    if (has) for (uint32_t v : st->list) styles.push_back(v & 0xFF);
    setCombo(style_, has, styles, StyleName, has ? (st->value & 0xFF) : 0, has && st->hasValue);
    uint32_t spProp = (has && st->hasValue) ? StylePropFor(st->value) : 0;
    const Prop* sp = spProp ? prop(spProp) : nullptr;
    std::vector<uint32_t> words;
    size_t base = 0;
    bool haveParams = sp && StyleParamWords(sp->raw, words, base);
    for (int i = 0; i < 4; i++) {
        EnableSlider(psSlider_[i], haveParams);
        if (haveParams) {
            int v;
            if (GetStyleParam(sp->raw, i, v)) SetSliderValue(psSlider_[i], v);
        } else {
            gtk_label_set_text(GTK_LABEL(psSlider_[i]->value), conn ? Tr(Str::NotAvailable) : "");
        }
    }
    // Auto Lighting Optimizer (only if the camera reports the value; this model reports 0..3)
    p = conn ? prop(kPropAlo) : nullptr;
    has = p && p->hasValue && p->value < 0x100;
    setCombo(alo_, has, std::vector<uint32_t>{0, 1, 2, 3}, AloName, has ? p->value : 0, has);
    gtk_widget_set_sensitive(af_, conn);
    updating_ = false;
}

// ---------------------------------------------------------------- preview

void Panel::OnPreview(const Json& j) {
    std::vector<uint8_t> jpeg = Base64Decode(j["jpeg"].Str());
    std::vector<uint8_t> rgb;
    int w, h;
    if (!DecodeJpegRgb(jpeg.data(), jpeg.size(), rgb, w, h)) return;
    if (!frame_ || frameW_ != w || frameH_ != h) {
        if (frame_) cairo_surface_destroy(frame_);
        frame_ = cairo_image_surface_create(CAIRO_FORMAT_RGB24, w, h);
        frameW_ = w;
        frameH_ = h;
    }
    cairo_surface_flush(frame_);
    uint8_t* dst = cairo_image_surface_get_data(frame_);
    int stride = cairo_image_surface_get_stride(frame_);
    for (int y = 0; y < h; y++) {
        auto* row = reinterpret_cast<uint32_t*>(dst + size_t(y) * size_t(stride));
        const uint8_t* s = &rgb[size_t(y) * size_t(w) * 3];
        for (int x = 0; x < w; x++, s += 3) row[x] = 0xFF000000u | uint32_t(s[0]) << 16 | uint32_t(s[1]) << 8 | s[2];
    }
    cairo_surface_mark_dirty(frame_);
    haveFrame_ = true;
    gtk_widget_queue_draw(area_);
}

double Panel::Scale(int w, int h) const {
    double fit = std::min(double(w) / frameW_, double(h) / frameH_);
    if (zoom_ == 0) return fit;
    return double(zoom_) * 1920.0 / frameW_;  // 100% = one source pixel of the 1920x1080 frame per screen pixel
}

void Panel::ClampPan(int w, int h) {
    if (!frameW_) return;
    double s = Scale(w, h);
    double iw = frameW_ * s, ih = frameH_ * s;
    double mx = std::max(0.0, (iw - w) / 2), my = std::max(0.0, (ih - h) / 2);
    panX_ = std::max(-mx, std::min(mx, panX_));
    panY_ = std::max(-my, std::min(my, panY_));
}

void Panel::SetZoom(int index) {
    if (index == zoom_) return;
    zoom_ = index;
    panX_ = panY_ = 0;
    if (!gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(zoomBtn_[index])))
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(zoomBtn_[index]), TRUE);
    // zoomed in, the panel asks for the full 1920 wide frame so checking focus is meaningful
    if (client_) client_->Subscribe(true, index == 0 ? 640 : 1920);
    gtk_widget_queue_draw(area_);
}

void Panel::DrawPreview(GtkDrawingArea*, cairo_t* cr, int w, int h, gpointer d) {
    auto* p = static_cast<Panel*>(d);
    cairo_set_source_rgb(cr, 0.08, 0.08, 0.08);
    cairo_paint(cr);
    if (!p->haveFrame_ || !p->frame_) {
        const char* text = p->connected_ ? Tr(Str::WaitingForCamera) : Tr(Str::ServiceNotRunning);
        cairo_set_source_rgb(cr, 0.7, 0.7, 0.7);
        cairo_select_font_face(cr, "sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
        cairo_set_font_size(cr, 15);
        cairo_text_extents_t ext;
        cairo_text_extents(cr, text, &ext);
        cairo_move_to(cr, (w - ext.width) / 2 - ext.x_bearing, h / 2.0);
        cairo_show_text(cr, text);
        return;
    }
    p->ClampPan(w, h);
    double s = p->Scale(w, h);
    double iw = p->frameW_ * s, ih = p->frameH_ * s;
    cairo_translate(cr, (w - iw) / 2 + p->panX_, (h - ih) / 2 + p->panY_);
    cairo_scale(cr, s, s);
    cairo_set_source_surface(cr, p->frame_, 0, 0);
    cairo_pattern_set_filter(cairo_get_source(cr), s >= 1.5 ? CAIRO_FILTER_NEAREST : CAIRO_FILTER_GOOD);
    cairo_rectangle(cr, 0, 0, p->frameW_, p->frameH_);
    cairo_fill(cr);
}

}  // namespace

// ---------------------------------------------------------------- application

static Panel* g_panel = nullptr;

static void OnActivate(GtkApplication* app, gpointer) {
    if (g_panel) {  // second launch: bring the first window to the front
        gtk_window_present(g_panel->window());
        return;
    }
    g_panel = new Panel(app);
    gtk_window_present(g_panel->window());
}

// --snapshot file.png [--preview frame.jpg] [--size WxH]: renders the panel with demo data, offscreen.
static int RunSnapshot(int argc, char** argv) {
    const char* out = nullptr;
    const char* preview = nullptr;
    int w = 1280, h = 800;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--snapshot") && i + 1 < argc) out = argv[++i];
        else if (!strcmp(argv[i], "--preview") && i + 1 < argc) preview = argv[++i];
        else if (!strcmp(argv[i], "--size") && i + 1 < argc) sscanf(argv[++i], "%dx%d", &w, &h);
    }
    if (!gtk_init_check()) { fprintf(stderr, "snapshot: no display\n"); return 1; }
    Panel panel(nullptr);
    panel.FeedState(true);
    const char* status = "{\"type\":\"status\",\"state\":\"live\",\"camera\":true,\"name\":\"Canon EOS REBEL T3\","
                         "\"fps_in\":29.9,\"fps_out\":29.9,\"ms\":7.8,\"consumers\":1,\"panels\":1,\"pipewire\":true}";
    const char* image = "{\"type\":\"image\",\"brightness\":10,\"contrast\":-20,\"gamma\":1.2,\"temperature\":5,\"tint\":0,\"saturation\":110,"
                        "\"upscale\":\"fsr\",\"sharpness\":0.2}";
    const char* cam = "{\"type\":\"camera\",\"gen\":3,\"connected\":true,\"name\":\"Canon EOS REBEL T3\",\"props\":{"
        "\"D103\":{\"value\":0,\"raw\":\"00000000\",\"list\":[]},"
        "\"D104\":{\"value\":11,\"raw\":\"0b000000\",\"list\":[232,235,237,240,243,245,248,251,253,0,3,5,8,11,13,16,19,21,24]},"
        "\"D109\":{\"value\":0,\"raw\":\"00000000\",\"list\":[0,1,8,2,3,4,5,6]},"
        "\"D110\":{\"value\":129,\"raw\":\"81000000\",\"list\":[129,130,131,132,133,134,33,34,35]},"
        "\"D150\":{\"value\":28,\"raw\":\"1c000000000000000200000000000000000000000000000000000000\"},"
        "\"D1C1\":{\"value\":2,\"raw\":\"02000000\"}}}";
    for (const char* m : {status, image, cam}) {
        Json j;
        if (Json::Parse(m, j)) panel.Feed(j);
    }
    if (preview) {
        std::string text;
        FILE* f = fopen(preview, "rb");
        if (f) {
            char buf[65536];
            size_t n;
            while ((n = fread(buf, 1, sizeof buf, f)) > 0) text.append(buf, n);
            fclose(f);
            Json j = Json::Object();
            j.Set("type", "preview");
            j.Set("jpeg", Base64Encode(reinterpret_cast<const uint8_t*>(text.data()), text.size()));
            panel.Feed(j);
        }
    }
    bool ok = out && panel.Snapshot(out, w, h);
    printf(ok ? "snapshot written to %s\n" : "snapshot failed\n", out ? out : "");
    return ok ? 0 : 1;
}

int main(int argc, char** argv) {
    InitLang(SettingsPath());
    for (int i = 1; i < argc; i++)
        if (!strcmp(argv[i], "--snapshot")) return RunSnapshot(argc, argv);
    GtkApplication* app = gtk_application_new("io.github.danarrib.CanonEosWebcamPanel", G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(app, "activate", G_CALLBACK(OnActivate), nullptr);
    int rc = g_application_run(G_APPLICATION(app), argc, argv);
    g_object_unref(app);
    return rc;
}
