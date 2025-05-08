### Oblivion Damage Formula Skill Cap Remover
Oblivion Remastered handles all skill capping in the luck skill modifier function. If a skill over 100 is inputted, it will be set back to 100 here.
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

This replaces the CMOVG instruction with a MOV instruction, removing the check for values less than or equal to 100, removing the cap.

## Install
Place the OBSE64 Plugin in
```
OblivionRemastered\Binaries\Win64\obse\plugins
```
then run the game through OBSE64.

Place the ASI Plugin in
```
OblivionRemastered\Binaries\Win64\plugins
or
OblivionRemastered\Binaries\WinGDK\plugins
```
then run the game normally.