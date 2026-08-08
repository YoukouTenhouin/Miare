# Design the encryption envelope and provider contract

Type: grilling
Status: open
Blocked by: 05, 06

## Question

How should v1 derive and separate internal keys from caller-supplied key material, authenticate bootstrap metadata and backend data, construct nonces, bind locations and versions as associated data, handle wrong keys and tampering, erase sensitive memory, rotate keys if supported, and abstract vetted providers without weakening the on-disk security contract?
