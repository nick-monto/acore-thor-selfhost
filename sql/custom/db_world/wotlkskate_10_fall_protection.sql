-- WotLK Skate fall protection for the custom AzerothCore realm.
--
-- This is the wotlkskate server/skate-fall-protection.sql rows, made re-appliable for the
-- core's CUSTOM SQL slot. No START TRANSACTION/COMMIT: the DB updater owns application,
-- and the existing CUSTOM files use plain re-appliable statements.
--
-- 90001: caster-only SAFE_FALL aura 144, 1,000,000 yd, DurationIndex 31 (8 s).
-- 90002: effect 164 removes only 90001 from the caster.
-- Rate.Damage.Fall remains stock; this is not a realm-wide fall-damage change.

INSERT INTO spell_dbc
 (ID, Attributes, DurationIndex, RangeIndex, EquippedItemClass,
  Effect_1, EffectDieSides_1, EffectBasePoints_1, ImplicitTargetA_1,
  EffectAura_1, Name_Lang_enUS, SchoolMask)
VALUES
 (90001,128,31,1,-1,6,1,999999,1,144,'Skate fall protection',1)
ON DUPLICATE KEY UPDATE
 Attributes=VALUES(Attributes),
 DurationIndex=VALUES(DurationIndex),
 RangeIndex=VALUES(RangeIndex),
 EquippedItemClass=VALUES(EquippedItemClass),
 Effect_1=VALUES(Effect_1),
 EffectDieSides_1=VALUES(EffectDieSides_1),
 EffectBasePoints_1=VALUES(EffectBasePoints_1),
 ImplicitTargetA_1=VALUES(ImplicitTargetA_1),
 EffectAura_1=VALUES(EffectAura_1),
 Name_Lang_enUS=VALUES(Name_Lang_enUS),
 SchoolMask=VALUES(SchoolMask);

INSERT INTO spell_dbc
 (ID, RangeIndex, EquippedItemClass, Effect_1, ImplicitTargetA_1,
  EffectTriggerSpell_1, Name_Lang_enUS, SchoolMask)
VALUES
 (90002,1,-1,164,1,90001,'End Skate fall protection',1)
ON DUPLICATE KEY UPDATE
 RangeIndex=VALUES(RangeIndex),
 EquippedItemClass=VALUES(EquippedItemClass),
 Effect_1=VALUES(Effect_1),
 ImplicitTargetA_1=VALUES(ImplicitTargetA_1),
 EffectTriggerSpell_1=VALUES(EffectTriggerSpell_1),
 Name_Lang_enUS=VALUES(Name_Lang_enUS),
 SchoolMask=VALUES(SchoolMask);
