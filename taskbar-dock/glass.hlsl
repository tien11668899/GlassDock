// S1mp1e's liquid glass for the taskbar dock: glass.fsh from S1mp1e
// (versions/mc263/src/main/resources/assets/liquidglass/shaders/core/glass.fsh, a port of
// reglass, which implements iyinchao's liquid-glass-studio model) with the same constants and the
// same steps, in target pixels with y down on both the target and the backdrop. Only the corners
// differ: a superellipse, which meets the straight edges with continuous curvature like the
// smoothed (Apple) corners the dock has always had; exponent 2 is glass.fsh's circular arc.
// build.cmd compiles it with fxc into out\glass_vs.h and out\glass_ps.h.

cbuffer Glass : register(b0)
{
    float2 centre;        // the dock's centre, target px
    float2 halfSize;      // its half width and height
    float2 captureOrigin; // where the captured backdrop's top-left pixel sits, target px
    float2 captureScale;  // 1 / the capture's size
    float corner;         // how far from each corner its curve starts, px
    float exponent;       // the corner curve: 2 = circular arc, more = continuous curvature
    float reach;          // glass.fsh's OFFSET_SCALE x screen height: the offset for an edge factor of 1
    float frost;          // px, glass.fsh's (1 - vertex alpha) x 4
    float shadowScale;    // 1 = glass.fsh's drop shadow
    float lift;           // glass.fsh's lift (1 - vertex green): a neutral white brighten
    float alpha;          // glass.fsh's vertex blue: the whole element's opacity
    float unused;
    float4 fill;          // instead of glass, a flat plate of this premultiplied colour (alpha > 0)
};

Texture2D backdrop : register(t0); // the desktop behind the dock, without the taskbar
SamplerState mirror : register(s0); // linear; mirrored past the edge, as past the screen's bottom

// reglass shipped defaults (physical px where noted), as in glass.fsh
static const float REF_THICKNESS = 13.15; // refraction band, px
static const float REF_FACTOR = 1.4;      // IOR
static const float REF_DISP = 7.0;        // dispersion knob
static const float FRES_RANGE = 39.71;
static const float FRES_HARD = 0.2;
static const float FRES_FAC = 0.2;
static const float SHADOW_EXPAND = 18.2;  // px
static const float SHADOW_FACTOR = 0.11;
static const float2 SHADOW_OFFSET = float2(0.0, 2.0); // px, +y = down

// One triangle that covers the whole target.
float4 FullScreen(uint id : SV_VertexID) : SV_Position
{
    float2 uv = float2((id << 1) & 2, id & 2);
    return float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
}

// The corner curve's radius through q (q >= 0): its length for a circle, the superellipse norm
// with exponent `e` else.
float CornerNorm(float2 q, float e)
{
    float m = max(q.x, q.y);
    if (m <= 0.0) return 0.0;
    float2 r = q / m;
    return m * pow(pow(r.x, e) + pow(r.y, e), 1.0 / e);
}

// sdgBox (reglass): signed distance (< 0 inside, px) to a rectangle `halfSize` around p's origin
// with corners `c` long on curve `e`, and the outward normal.
float RoundedBox(float2 p, float2 half, float c, float e, out float2 n)
{
    float2 bb = half - c;
    float2 w = abs(p) - bb;
    float2 s = float2(p.x < 0.0 ? -1.0 : 1.0, p.y < 0.0 ? -1.0 : 1.0);
    float g = max(w.x, w.y);
    float2 q = max(w, 0.0);
    float l = CornerNorm(q, e);
    float d = (g > 0.0) ? l - c : g - c;
    float2 grad = pow(q, e - 1.0); // the curve's gradient; q itself for a circle
    n = (g > 0.0) ? grad / max(length(grad), 1e-6) : ((w.x > w.y) ? float2(1.0, 0.0) : float2(0.0, 1.0));
    n *= s;
    return d;
}

// The element being drawn.
float Distance(float2 p, out float2 n)
{
    return RoundedBox(p, halfSize, corner, exponent, n);
}

float4 Behind(float2 at)
{
    return backdrop.SampleLevel(mirror, (at - captureOrigin) * captureScale, 0.0);
}

float4 Glass(float4 position : SV_Position) : SV_Target
{
    float2 p = position.xy - centre;
    float2 n;
    float d = Distance(p, n);

    // The hover highlight: a flat plate, antialiased the way taskbar_hooks.dll draws its plates.
    if (fill.a > 0.0) return fill * saturate(0.5 - d);

    float aa = max(fwidth(d), 1e-4);
    float cov = 1.0 - smoothstep(-aa, aa, d);

    // drop shadow: the distance shifted by the shadow offset; exp falloff.
    float2 shadowNormal;
    float ds = Distance(p - SHADOW_OFFSET, shadowNormal);
    float shadow = exp(-abs(ds) / SHADOW_EXPAND) * 0.6 * SHADOW_FACTOR * shadowScale;

    if (cov <= 0.001 && shadow <= 0.004) return 0.0;

    // Snell edge factor (identical math to reglass/iyinchao)
    float nmerged = max(-d, 0.0); // px inside
    float xR = 1.0 - nmerged / REF_THICKNESS;
    float thetaI = asin(pow(saturate(xR), 2.0));
    float thetaT = asin(clamp(sin(thetaI) / REF_FACTOR, -1.0, 1.0));
    float edgeFactor = -tan(thetaT - thetaI);
    if (nmerged >= REF_THICKNESS) edgeFactor = 0.0;

    // glass.fsh's -normal x edge x 0.08 x (H/W, 1) in UV is -normal x edge x 0.08 x H in pixels.
    float2 refrOffset = -n * edgeFactor * reach;

    // per-channel dispersion of the refraction offset (reglass: NR .985 / NG 1 / NB 1.015)
    float2 offR = refrOffset * (1.0 - (0.985 - 1.0) * REF_DISP);
    float2 offG = refrOffset;
    float2 offB = refrOffset * (1.0 - (1.015 - 1.0) * REF_DISP);

    float2 base = position.xy;
    float3 col;
    if (frost > 0.1) {
        col = 0.0;
        float wsum = 0.0;
        [unroll] for (int j = -1; j <= 1; j++) {
            [unroll] for (int i = -1; i <= 1; i++) {
                float wg = (i == 0 && j == 0) ? 4.0 : ((i == 0 || j == 0) ? 2.0 : 1.0);
                float2 o = float2(i, j) * frost;
                col.r += Behind(base + offR + o).r * wg;
                col.g += Behind(base + offG + o).g * wg;
                col.b += Behind(base + offB + o).b * wg;
                wsum += wg;
            }
        }
        col /= wsum;
    } else {
        col = float3(Behind(base + offR).r, Behind(base + offG).g, Behind(base + offB).b);
    }

    // Neutral luminance lift: not a tint, a pure white brighten, how S1mp1e's hotbar selection
    // separates from the strip.
    col = lerp(col, 1.0, lift);

    // Fresnel white rim (reglass formula); the lifted element gets a stronger rim.
    float fres = saturate(pow(max(1.0 + d / 1500.0 * pow(500.0 / FRES_RANGE, 2.0) + FRES_HARD, 0.0), 5.0));
    col = lerp(col, 1.0, fres * FRES_FAC * 0.7 * (1.0 + lift * 8.0));

    // blur + refraction + shadow + edge light. Nothing else: no tint of any kind.
    float4 c = lerp(float4(0.0, 0.0, 0.0, shadow), float4(col, 1.0), cov);
    c.a *= alpha;
    // Minecraft blends this with (src alpha, 1 - src alpha); composition takes premultiplied colour.
    return float4(c.rgb * c.a, c.a);
}

// ---- The Start menu sliding back under the dock ----
//
// start-glass slides the menu out from under the dock; closing, the shell hides it at once, so the
// dock draws a picture of it (taken while it was open) going back the same way: scaled evenly about
// its bottom middle from the menu's width down to the dock's while that point sinks as far below
// the menu's bottom edge (the top of the taskbar, where the picture's window ends) as the scaled
// picture is tall, fading out over the last part. Progress 1 is the menu, 0 gone under the dock.

cbuffer Ghost : register(b1)
{
    float4 menuRect;      // where the picture is: left, top, right, bottom, target px
    float4 barRect;       // the dock-sized shape it goes back to
    float progress;
    float menuRadius;     // corner radii, px
    float barRadius;
    float ghostExponent;  // the corner curve (see Exponent in dock.cpp)
    float fadeIn;         // fully opaque from this much progress up
    float cornerScale;    // a corner's length over its radius: 1 + smoothing
    float2 ghostUnused;
};

Texture2D picture : register(t1);

float4 GhostPicture(float4 position : SV_Position) : SV_Target
{
    float2 size = menuRect.zw - menuRect.xy;
    float barScale = (barRect.z - barRect.x) / size.x;
    float scale = lerp(barScale, 1.0, progress);
    // The picture's bottom middle: from the menu's down under the dock's middle.
    float2 home = float2((menuRect.x + menuRect.z) / 2, menuRect.w);
    float2 anchor = float2(lerp((barRect.x + barRect.z) / 2, home.x, progress), home.y + (1.0 - progress) * barScale * size.y);

    // The picture's outline, scaled: the menu's rounded rectangle.
    float2 half = size * scale / 2;
    float corner = min(menuRadius * scale * cornerScale, min(half.x, half.y)); // as Draw makes the dock's
    float2 normal;
    float d = RoundedBox(position.xy - (anchor - float2(0.0, half.y)), half, corner, ghostExponent, normal);
    float aa = max(fwidth(d), 1e-4);
    float cov = 1.0 - smoothstep(-aa, aa, d);
    if (cov <= 0.001) return 0.0;

    float2 at = home + (position.xy - anchor) / scale;
    float3 col = picture.SampleLevel(mirror, (at - menuRect.xy) / size, 0.0).rgb;
    float a = cov * saturate(progress / fadeIn);
    return float4(col * a, a);
}
