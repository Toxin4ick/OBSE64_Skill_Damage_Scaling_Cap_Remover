This hooks into the SpellCost function and adds a new scaling function for skills over 100.

    float ModifiedSkill = g_luck_calculator(Skill, Luck);
    float calculatedCost;

    if (ModifiedSkill > 100.0f) {
        calculatedCost = (Base_Cost * fMagicCasterSkillCostBase) * (100.0f / ModifiedSkill);
    }
    else { 
        calculatedCost = ((1.0f - ModifiedSkill / 100.0f) * fMagicCasterSkillCostMult + fMagicCasterSkillCostBase) * Base_Cost;
    }

    // Return Result
    return calculatedCost;