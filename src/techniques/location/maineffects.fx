technique DLightModel
{
    pass p0
    {
        Lighting = true;
    }
}

technique LighthouseLight
{
    pass p0
    {
        Lighting = false;
        FogEnable = false;
        AlphaTestEnable = false;
        AlphaBlendEnable = true;
        ZWriteEnable = false;
        SrcBlend = srcalpha;
        DestBlend = one;
        ColorArg1[0] = texture;
        ColorOp[0] = SelectArg1;
        AlphaArg1[0] = texture;
        AlphaOp[0] = SelectArg1;
    }
}

technique LocationWaterFall
{
    pass p0
    {
        AlphaTestEnable = false;
        AlphaBlendEnable = true;
        SrcBlend = srcalpha;
        DestBlend = invsrcalpha;
        ZWriteEnable = false;

        ColorArg1[0] = diffuse;
        ColorArg2[0] = texture;
        ColorOp[0] = modulate2x;
        ColorArg1[1] = current;
        ColorOp[1] = SelectArg1;
        ColorOp[2] = disable;
        AlphaArg1[0] = texture;
        AlphaOp[0] = SelectArg1;
        AlphaArg1[1] = current;
        AlphaArg2[1] = texture;
        AlphaOp[1] = modulate;
        AlphaOp[2] = disable;
    }
}

technique LocationWindows
{
    pass p0
    {
        AlphaArg1[0] = texture;
        AlphaOp[0] = selectarg1;
        AlphaOp[1] = disable;
        AlphaTestEnable = false;
        AlphaBlendEnable = true;
        SrcBlend = srcalpha;
        DestBlend = invsrcalpha;
        ZWriteEnable = false;
    }
}

technique LocationModelBlend
{
    pass p0
    {
        Lighting = false;
        FogEnable = true;
        AlphaTestEnable = false;
        AlphaBlendEnable = true;
        SrcBlend = srcalpha;
        DestBlend = invsrcalpha;
        ZEnable = true;
        ZWriteEnable = false;
        ColorArg1[0] = texture;
        ColorArg2[0] = diffuse;
        ColorOp[0] = modulate2x;
        ColorOp[1] = disable;
        AlphaArg1[0] = texture;
        AlphaArg2[0] = diffuse;
        AlphaOp[0] = modulate;
        AlphaOp[1] = disable;
    }
}

technique ShootParticles
{
    pass p0
    {
        Lighting = false;
        FogEnable = true;
        CullMode = none;
        AlphaTestEnable = false;
        AlphaBlendEnable = true;
        SrcBlend = srcalpha;
        DestBlend = invsrcalpha;
        ZEnable = true;
        ZWriteEnable = false;
        ColorArg1[0] = texture;
        ColorArg2[0] = diffuse;
        ColorOp[0] = modulate;
        ColorOp[1] = disable;
        AlphaArg1[0] = texture;
        AlphaArg2[0] = diffuse;
        AlphaOp[0] = modulate;
        AlphaOp[1] = disable;
    }
}

technique ShootParticlesNoZ
{
    pass p0
    {
        Lighting = false;
        FogEnable = true;
        CullMode = none;
        AlphaTestEnable = false;
        AlphaBlendEnable = true;
        SrcBlend = srcalpha;
        DestBlend = invsrcalpha;
        ZEnable = false;
        ZWriteEnable = false;
        ColorArg1[0] = texture;
        ColorArg2[0] = diffuse;
        ColorOp[0] = modulate;
        ColorOp[1] = disable;
        AlphaArg1[0] = texture;
        AlphaArg2[0] = diffuse;
        AlphaOp[0] = modulate;
        AlphaOp[1] = disable;
    }
}

technique EnvironmentShader
{
    pass p0
    {
        Lighting = false;

        ColorArg1[0] = diffuse;
        ColorArg2[0] = texture;
        ColorOp[0] = modulate2x;
        ColorArg2[1] = current;
        ColorArg1[1] = texture;
        ColorOp[1] = blendcurrentalpha;

        ColorOp[2] = disable;

        AlphaArg1[0] = texture;
        AlphaArg2[0] = tfactor;
        AlphaOp[0] = modulate;
        AlphaOp[1] = disable;

        TexCoordIndex[1] = cameraspacereflectionvector;
        TextureTransformFlags[1] = count3;
        LocalViewer = true;
        NormalizeNormals = true;

        AlphaTestEnable = false;
        AlphaBlendEnable = false;
    }
}

technique EnvAmmoShader
{
    pass p0
    {
        Lighting = true;

        ColorArg1[0] = diffuse;
        ColorArg2[0] = texture;
        ColorOp[0] = modulate2x;
        ColorArg2[1] = current;
        ColorArg1[1] = texture;
        ColorOp[1] = blendcurrentalpha;
        ColorOp[2] = disable;

        AlphaArg1[0] = texture;
        AlphaOp[0] = SelectArg1;
        AlphaArg1[1] = current;
        AlphaOp[1] = SelectArg1;
        AlphaOp[2] = disable;

        TexCoordIndex[1] = cameraspacereflectionvector;
        TextureTransformFlags[1] = count3;
        LocalViewer = true;
        NormalizeNormals = true;

        AlphaTestEnable = false;
        AlphaBlendEnable = false;
    }
}

technique LocCharacterSplashes
{
    pass p0
    {
        Lighting = false;
        FogEnable = true;
        CullMode = none;
        ZWriteEnable = false;
        ZEnable = true;
        AlphaTestEnable = false;
        AlphaBlendEnable = true;
        SrcBlend = srcalpha;
        DestBlend = invsrcalpha;
        ColorArg1[0] = texture;
        ColorArg2[0] = current;
        ColorOp[0] = modulate;
        ColorOp[1] = disable;
        AlphaArg1[0] = texture;
        AlphaArg2[0] = current;
        AlphaOp[0] = modulate;
        AlphaOp[1] = disable;
    }
}

technique LocVisRays
{
    pass p0
    {
        Lighting = false;
        FogEnable = true;
        ZWriteEnable = false;
        ZEnable = true;
        AlphaTestEnable = false;
        AlphaBlendEnable = true;
        SrcBlend = srcalpha;
        DestBlend = one;
        ColorArg1[0] = texture;
        ColorArg2[0] = current;
        ColorOp[0] = selectArg1;
        ColorArg1[1] = texture;
        ColorArg2[1] = current;
        ColorOp[1] = modulate;
        ColorOp[2] = disable;
        AlphaArg1[0] = texture;
        AlphaArg2[0] = current;
        AlphaOp[0] = selectArg1;
        AlphaOp[1] = disable;
    }
}
