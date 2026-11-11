// Dirección del pellizco en el trackpad (sólo macOS).
// macOS avisa el pellizco (magnify) sin decir si los dedos se abren en horizontal o en vertical,
// así que se miran los dos dedos al empezar el gesto: si están más separados en vertical que en
// horizontal, es un pellizco vertical.
#import <AppKit/AppKit.h>
#include <atomic>
#include <cmath>

static std::atomic<int> gPinchAxis { 0 }; // 0 = no se sabe, 1 = horizontal, 2 = vertical
static id gPinchMonitor = nil;

int cyclenormPinchAxis() { return gPinchAxis.load(); }

void cyclenormInstallPinchMonitor()
{
    if (gPinchMonitor != nil) return;
    gPinchMonitor = [NSEvent addLocalMonitorForEventsMatchingMask: NSEventMaskMagnify
                                                          handler: ^NSEvent* (NSEvent* e)
    {
        if (e.phase == NSEventPhaseBegan || e.phase == NSEventPhaseMayBegin)
            gPinchAxis = 0;
        // que la ventana reciba los toques del trackpad, así el pellizco trae la posición de los dedos
        if (NSView* v = e.window.contentView)
            if ((v.allowedTouchTypes & NSTouchTypeMaskIndirect) == 0)
                v.allowedTouchTypes = v.allowedTouchTypes | NSTouchTypeMaskIndirect;
        if (gPinchAxis.load() == 0)
        {
            NSSet<NSTouch*>* touches = [e touchesMatchingPhase: NSTouchPhaseTouching inView: nil];
            if (touches.count == 2)
            {
                NSArray<NSTouch*>* t = [touches allObjects];
                const NSPoint p0 = t[0].normalizedPosition, p1 = t[1].normalizedPosition;
                NSSize sz = t[0].deviceSize; // en puntos; el trackpad es más ancho que alto
                if (sz.width <= 0 || sz.height <= 0) sz = NSMakeSize (1.6, 1.0);
                const double dx = std::abs (p0.x - p1.x) * sz.width;
                const double dy = std::abs (p0.y - p1.y) * sz.height;
                gPinchAxis = dy > dx ? 2 : 1;
            }
        }
        return e;
    }];
}
