module SolidColorPattern implements IMaterialPattern
{
    param vec3 solidColor;
    
    public vec3 albedo = solidColor;
    public vec3 normal = vec3(0.0, 0.0, 1.0);
    public float specular = 0.3;
    public float roughness = 0.7;
    public float metallic = 0.4;
    public float selfShadow(vec3 lightDir)
    {
        return 1.0;
    }
}