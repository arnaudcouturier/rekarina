#version 450
layout(location = 0) in vec2 texcoord;
layout(location = 0) out vec4 color;
#ifdef SECOND_OUTPUT
layout(location = 1) out vec4 other_color;
#endif
void main() {
    if (texcoord.x < 0.0)
        discard;
    float alpha = texcoord.y < 0.5 ? 0.25 : 0.5;
#ifdef COMPONENT_OUTPUT
    color.r = 1.0;
    color.g = 1.0;
    color.b = 1.0;
    color.a = alpha;
#else
    if (texcoord.y < 0.5)
        color = vec4(1.0, 1.0, 1.0, 0.25);
    else
        color = vec4(1.0, 1.0, 1.0, 0.5);
#endif
#ifdef SECOND_OUTPUT
    other_color = vec4(1.0);
#endif
}
