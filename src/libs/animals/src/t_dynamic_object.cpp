#include "t_dynamic_object.h"
#include "rands.h"

TDynamicObject::TDynamicObject()
{
}

TDynamicObject::~TDynamicObject()
{
}

void TDynamicObject::Initialize(const glm::vec3 &_center, float _radius)
{
    pos.x = _center.x + randCentered(_radius);
    pos.y = 0.0f;
    pos.z = _center.z + randCentered(_radius);
    velocity = storm::math::normalizeOrZero(glm::vec3(randCentered(1.0f), 0.0f, randCentered(1.0f)));
}

void TDynamicObject::Calculate(TDynamicObject **a, int aCount, TDynamicObject **d, int dCount, float _k)
{

    glm::vec3 aEffect(0.0f, 0.0f, 0.0f);
    glm::vec3 dEffect(0.0f, 0.0f, 0.0f);
    TDynamicObject **effectObject;
    int i;

    for (i = 0, effectObject = a; i < aCount; i++, effectObject++)
    {
        if (!(*effectObject) || (this == *effectObject))
            continue;

        aEffect += ATTRACT_FACTOR * ((*effectObject)->pos - pos);
    }

    for (i = 0, effectObject = d; i < dCount; i++, effectObject++)
    {
        if (!(*effectObject) || (this == *effectObject))
            continue;

        glm::vec3 delta;
        delta = (*effectObject)->pos - pos;
        auto d = static_cast<float>(sqrt(storm::math::lengthSquared(delta)));
        if (d < 1e-10f)
            d = 1e-10f;
        dEffect += DEFLECT_FACTOR * storm::math::normalizeOrZero(delta) / d;
    }

    velocity = storm::math::normalizeOrZero(velocity + MOVEMENT_FACTOR * storm::math::normalizeOrZero(aEffect - dEffect));
    pos += _k * velocity;

}

void TDynamicObject::SetXYZ(const glm::vec3 &_pos)
{
    pos = _pos;
}
