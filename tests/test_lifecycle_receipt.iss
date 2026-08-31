[Setup]
AppId=EVOX2-Lifecycle-Receipt-Contract
AppName=EVO-X2 Lifecycle Receipt Contract
AppVersion=1.0.0
DefaultDirName={tmp}\EVOX2 Lifecycle Receipt Contract
CreateAppDir=no
Uninstallable=no
PrivilegesRequired=lowest
DisableProgramGroupPage=yes
DisableStartupPrompt=yes
OutputDir=.
OutputBaseFilename=EVOX2-Lifecycle-Receipt-Contract
Compression=none

[Code]
#include "..\installer\lifecycle-receipt.iss"

procedure ExpectValidReceipt(const Value, Prefix, LabelText: AnsiString);
var
  Actual: AnsiString;
begin
  Actual := ValidateLifecycleReceipt(Value, Prefix);
  if Actual <> Value then
    RaiseException('Valid receipt changed: ' + LabelText);
end;

procedure ExpectValidReceiptWithTerminator(
  const Value, Expected, Prefix, LabelText: AnsiString);
var
  Actual: AnsiString;
begin
  Actual := ValidateLifecycleReceipt(Value, Prefix);
  if Actual <> Expected then
    RaiseException('Valid terminated receipt changed: ' + LabelText);
end;

procedure ExpectInvalidReceipt(const Value, Prefix, LabelText: AnsiString);
var
  Actual: AnsiString;
  Rejected: Boolean;
begin
  Rejected := False;
  try
    Actual := ValidateLifecycleReceipt(Value, Prefix);
  except
    Rejected := True;
  end;
  if not Rejected then
    RaiseException('Malformed receipt was accepted: ' + LabelText + ' ' + Actual);
end;

procedure CurStepChanged(CurStep: TSetupStep);
var
  Prefix, Valid: AnsiString;
begin
  if CurStep <> ssPostInstall then
    exit;

  Prefix := 'AUTOSTART_TASK=PASS EVO-X2 P-MODE Widget S-1-';
  Valid := Prefix + '5-21-4025080884-680144943-2562851548-1002';
  ExpectValidReceipt(Valid, Prefix, 'canonical');
  ExpectValidReceiptWithTerminator(Valid + #13#10, Valid, Prefix, 'crlf');
  ExpectValidReceiptWithTerminator(Valid + #10, Valid, Prefix, 'lf');
  ExpectValidReceiptWithTerminator(Valid + #13, Valid, Prefix, 'cr');

  ExpectInvalidReceipt('', Prefix, 'empty');
  ExpectInvalidReceipt(Prefix, Prefix, 'missing-sid');
  ExpectInvalidReceipt(Prefix + '-', Prefix, 'empty-authority');
  ExpectInvalidReceipt(Prefix + '5-', Prefix, 'trailing-separator');
  ExpectInvalidReceipt(Prefix + '-5', Prefix, 'leading-separator');
  ExpectInvalidReceipt(Prefix + '5--21', Prefix, 'empty-component');
  ExpectInvalidReceipt(Prefix + '5-a', Prefix, 'non-numeric-component');
  ExpectInvalidReceipt(Lowercase(Valid), Prefix, 'case-variant');
  ExpectInvalidReceipt(Valid + ' trailing', Prefix, 'trailing-content');
  ExpectInvalidReceipt(Valid + #13#10 + 'extra', Prefix, 'multiline');

  Log('LIFECYCLE_RECEIPT_VALIDATOR=PASS');
end;
