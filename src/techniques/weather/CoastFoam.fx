technique CoastFoam
{
    pass p0
    {
        ZWriteEnable = false;
        FogEnable = false;
        Lighting = false;
        CullMode = none;
        AlphaTestEnable = false;
        AlphaBlendEnable = true;
        SrcBlend = srcalpha;
        DestBlend = invsrcalpha;

        // Stage 0: the foam texture (colour = texture, alpha = texture * diffuse * 2, fs_port_coastfoam);
        // stage 1: the sea's height frames, sampled by the vertex shader (vs_port_coastfoam)
        MinFilter[1] = linear;
        MagFilter[1] = linear;
        MipFilter[1] = none;
        AddressU[1] = wrap;
        AddressV[1] = wrap;
        AddressW[1] = wrap;

        VertexShader = FOAM_VERTEX_SHADER;
        PixelShader = FOAM_PIXEL_SHADER;
    }
}
