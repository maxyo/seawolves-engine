technique shadow_model
{
    pass p0
    {
        AlphaTestEnable = false;
        AlphaBlendEnable = false;
        FogEnable = false;

        ColorOp[0] = SelectArg1;
        ColorArg1[0] = TFactor;

        ColorOp[1] = disable;
    }
}
