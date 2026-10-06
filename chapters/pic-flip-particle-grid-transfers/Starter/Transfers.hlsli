// Stage zero deliberately performs identity transfer; unfinished helpers are gated off.
struct Stencil { uint face[8]; float weight[8]; uint count; };
Stencil Support(float3 position, uint axis)
{
    return (Stencil)0;
}
void AddFloat(uint address, float value)
{
}
void ScatterParticle(Particle p, uint axis, Stencil s)
{
}
void NormalizeFace(uint face)
{
}
float Gather(Stencil s, bool delta)
{
    return 0;
}
float3 TransferVelocity(Particle p, float3 pic, float3 delta)
{
    return p.velocity;
}
