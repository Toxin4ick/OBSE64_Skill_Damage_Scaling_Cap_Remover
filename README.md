### Oblivion Damage Formula Skill Cap Remover
Oblivion Remastered handles all skill capping in the luck skill modifier calculator. If a skill over 100 is inputted, it will be set back to 100 here.
``` 
float __fastcall luck_skill_modifier(int a1_SkillInQuestion, int a2_Luck)
{
  float *v2; // rax
  float result; // xmm0_4
  float v4; // [rsp+8h] [rbp+8h] BYREF
  float v5; // [rsp+10h] [rbp+10h] BYREF

  v2 = &v4;
  v4 = 100.0;
  result = 0.0;
  v5 = (float)((float)((float)a2_Luck * *(float *)&fActorLuckSkillMult) + (float)iActorLuckSkillBase)
     + (float)a1_SkillInQuestion;
  if ( v5 <= 100.0 )
    v2 = &v5;
  if ( *v2 >= 0.0 )
    return *v2;
  return result;
}
``` 
The Assembly Instruction:
``` 
1468CA02B 48 0F 46 C1       CMOVG      EBX ,EAX
``` 

This replaces the CMOVG instruction with a MOV instruction, removing the check for values less than 100, removing the cap.

## Build

### Requirements
* [XMake](https://xmake.io) [2.8.2+]
* C++23 Compiler (MSVC, Clang-CL)

```
git clone --recurse-submodules https://github.com/Baestus/OBSE64_Skill_Damage_Scaling_Cap_Remover.git
``` 
``` 
cd OBSE64_Skill_Damage_Scaling_Cap_Remover
```
``` 
xmake build
```

## Install
Place the EffectiveSkillDamageUncapper.dll in
``` 
OblivionRemastered\Binaries\Win64\obse\plugins
```
then run the game through OBSE64.