technique PostProcess
{
    pass p0
    {
        ZEnable = false;
        AlphaTestEnable = false;
        AlphaBlendEnable = false;
        FogEnable = false;

        ColorOp[0] = SelectArg1;
        ColorArg1[0] = Texture;

        ColorOp[1] = disable;
    }
}
