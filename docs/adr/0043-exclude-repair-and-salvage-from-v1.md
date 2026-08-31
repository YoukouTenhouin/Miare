# Exclude repair, salvage, and degraded reads from v1

V1 recovery only selects and validates a complete committed generation: it may ignore an incomplete protection-invalid publication, but corruption beneath a protection-valid publication fails closed. Verification diagnoses without mutation, and v1 provides no repair, partial salvage, or degraded read-only continuation because those workflows would need new public semantics for protection guarantees, omissions, and referential damage; a future separately named offline tool may define them without weakening normal open behavior.
