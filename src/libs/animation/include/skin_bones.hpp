#pragma once
// The bones of a skeleton whose skin shows: the head (with the neck, the face, the hair and everything under the neck
// joint) and both hands (with the fingers). The relief of the characters (renderer_bgfx ff_fragment.sh) fades out
// where these bones move the vertices: PBRify reads the painted shading of faces, hands and hair as fake bumps, and
// a colour mask misses dark skin. The skin weight of a vertex is its blend of these flags
// (model gpu_skinning.hpp writes it into the GPU copy of the vertex).
//
// The game's .an skeletons have no bone names (an_file.h) and the "Head bone index" of the .ani files is not the head
// in every skeleton, so the bones are found from the rest pose, a T-pose in all of TEHO's human skeletons (37 of 42
// files; animals, crabs and birds have none):
//   * the shoulder joint: three children or more, one whose bones rise above it and stay near the middle (the neck
//     and head: its whole subtree is skin), two whose bones reach out to either side (the arms);
//   * a hand: along an arm the chain that reaches furthest out, the last joint of it that branches (into the fingers;
//     the twist bones of the upper and lower arm branch before it); its subtree is skin.
#include "storm_glm.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace storm::animation
{

struct SkinBones
{
    std::vector<uint8_t> skin; // per bone: 1 skin, 0 not; empty when the skeleton has no humanoid T-pose
    // Per bone: 1 the torso and the arms (the spine from the shoulder joint down to the hips, the shoulder joint's subtree
    // without the neck and the hands): what shirts and coats cover as often as bare skin (fx/skin_tone.hpp decides by
    // colour there). Empty with `skin`.
    std::vector<uint8_t> torso;
    int shoulder = -1, neck = -1, hands[2] = {-1, -1};
    std::string failure; // why `skin` is empty
};

// `parents`: the .an file's parent index per joint (-1 for the root); `offsets`: its start positions, each relative to
// the parent's.
inline SkinBones FindSkinBones(std::span<const int32_t> parents, std::span<const glm::vec3> offsets)
{
    SkinBones result;
    const size_t n = parents.size();
    if (n == 0 || offsets.size() != n)
    {
        result.failure = "no joints";
        return result;
    }
    std::vector<glm::vec3> at(n);
    std::vector<std::vector<int>> children(n);
    for (size_t i = 0; i < n; ++i)
    {
        const int32_t p = parents[i];
        if (p >= static_cast<int32_t>(i) || (i > 0 && p < 0))
        {
            result.failure = "joint " + std::to_string(i) + " has its parent after it";
            return result;
        }
        at[i] = p < 0 ? offsets[i] : at[p] + offsets[i];
        if (p >= 0)
            children[p].push_back(static_cast<int>(i));
    }
    // Per subtree: the highest point and the furthest out to either side (joints come after their parents).
    std::vector<float> top(n), left(n), right(n);
    for (size_t k = 0; k < n; ++k)
    {
        top[k] = at[k].y;
        left[k] = right[k] = at[k].x;
    }
    for (size_t k = n; k-- > 1;)
    {
        const int32_t p = parents[k];
        top[p] = std::max(top[p], top[k]);
        left[p] = std::min(left[p], left[k]);
        right[p] = std::max(right[p], right[k]);
    }
    constexpr float kArmReach = 0.3f, kNeckRise = 0.15f, kHeadSide = 0.35f;
    for (size_t j = 0; j < n && result.shoulder < 0; ++j)
    {
        if (children[j].size() < 3)
            continue;
        int up = -1, arm_left = -1, arm_right = -1;
        for (const int c : children[j])
        {
            if (top[c] > at[j].y + kNeckRise && std::max(-left[c], right[c]) < kHeadSide)
                up = c;
            else if (left[c] < -kArmReach && right[c] < kArmReach)
                arm_left = c;
            else if (right[c] > kArmReach && left[c] > -kArmReach)
                arm_right = c;
        }
        if (up >= 0 && arm_left >= 0 && arm_right >= 0)
        {
            result.shoulder = static_cast<int>(j);
            result.neck = up;
            result.hands[0] = arm_left;
            result.hands[1] = arm_right;
        }
    }
    if (result.shoulder < 0)
    {
        result.failure = "no shoulder joint with a neck and two arms in the rest pose";
        return result;
    }
    for (int side = 0; side < 2; ++side)
    {
        int joint = result.hands[side], hand = -1;
        while (joint >= 0)
        {
            if (children[joint].size() >= 2)
                hand = joint;
            int next = -1;
            float reach = 0.0f;
            for (const int c : children[joint])
            {
                const float r = side == 0 ? -left[c] : right[c];
                if (next < 0 || r > reach)
                {
                    next = c;
                    reach = r;
                }
            }
            joint = next;
        }
        if (hand < 0)
        {
            result.failure = std::string("the ") + (side == 0 ? "left" : "right") + " arm has no hand that branches";
            return result;
        }
        result.hands[side] = hand;
    }
    // The torso: the shoulder joint's subtree, and the spine below it down to the joint where the legs branch off.
    result.torso.assign(n, 0);
    {
        std::vector<int> stack{result.shoulder};
        while (!stack.empty())
        {
            const int j = stack.back();
            stack.pop_back();
            result.torso[j] = 1;
            stack.insert(stack.end(), children[j].begin(), children[j].end());
        }
        constexpr float kLegReach = 0.5f; // a child subtree reaching this far below the joint is a leg
        for (int j = parents[result.shoulder]; j >= 0; j = parents[j])
        {
            bool legs = false;
            std::vector<int> stack2{j};
            float low = at[j].y;
            while (!stack2.empty())
            {
                const int k = stack2.back();
                stack2.pop_back();
                if (result.torso[k])
                    continue;
                low = std::min(low, at[k].y);
                stack2.insert(stack2.end(), children[k].begin(), children[k].end());
            }
            legs = low < at[j].y - kLegReach;
            if (legs)
                break;
            result.torso[j] = 1;
        }
    }
    result.skin.assign(n, 0);
    for (const int root : {result.neck, result.hands[0], result.hands[1]})
    {
        std::vector<int> stack{root};
        while (!stack.empty())
        {
            const int j = stack.back();
            stack.pop_back();
            result.skin[j] = 1;
            result.torso[j] = 0;
            stack.insert(stack.end(), children[j].begin(), children[j].end());
        }
    }
    return result;
}

} // namespace storm::animation
