function ValidateLifecycleReceipt(const Value, ExpectedPrefix: AnsiString): AnsiString;
var
  Receipt: AnsiString;
  FirstSidIndex, I: Integer;
begin
  Receipt := Value;
  if (Length(Receipt) >= 2) and
     (Receipt[Length(Receipt) - 1] = #13) and
     (Receipt[Length(Receipt)] = #10) then
    Delete(Receipt, Length(Receipt) - 1, 2)
  else if (Length(Receipt) >= 1) and
          ((Receipt[Length(Receipt)] = #13) or (Receipt[Length(Receipt)] = #10)) then
    Delete(Receipt, Length(Receipt), 1);

  if (ExpectedPrefix = '') or
     (Receipt = '') or
     (Pos(#13, Receipt) <> 0) or
     (Pos(#10, Receipt) <> 0) or
     (Copy(Receipt, 1, Length(ExpectedPrefix)) <> ExpectedPrefix) or
     (Length(Receipt) = Length(ExpectedPrefix)) then
    RaiseException('Autostart lifecycle receipt is malformed.');

  FirstSidIndex := Length(ExpectedPrefix) + 1;
  if not ((Receipt[FirstSidIndex] >= '0') and (Receipt[FirstSidIndex] <= '9')) or
     not ((Receipt[Length(Receipt)] >= '0') and (Receipt[Length(Receipt)] <= '9')) then
    RaiseException('Autostart lifecycle receipt SID is malformed.');

  for I := FirstSidIndex to Length(Receipt) do
  begin
    if not ((Receipt[I] >= '0') and (Receipt[I] <= '9')) then
    begin
      if (Receipt[I] <> '-') or
         (I = FirstSidIndex) or
         (Receipt[I - 1] = '-') then
        RaiseException('Autostart lifecycle receipt SID is malformed.');
    end;
  end;
  Result := Receipt;
end;
