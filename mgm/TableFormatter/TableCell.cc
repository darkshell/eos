//------------------------------------------------------------------------------
// File: TableCell.cc
// Author: Ivan Arizanovic & Stefan Isidorovic - Comtrade
//------------------------------------------------------------------------------

/************************************************************************
 * EOS - the CERN Disk Storage System                                   *
 * Copyright (C) 2017 CERN/Switzerland                                  *
 *                                                                      *
 * This program is free software: you can redistribute it and/or modify *
 * it under the terms of the GNU General Public License as published by *
 * the Free Software Foundation, either version 3 of the License, or    *
 * (at your option) any later version.                                  *
 *                                                                      *
 * This program is distributed in the hope that it will be useful,      *
 * but WITHOUT ANY WARRANTY; without even the implied warranty of       *
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the        *
 * GNU General Public License for more details.                         *
 *                                                                      *
 * You should have received a copy of the GNU General Public License    *
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.*
 ************************************************************************/

#include "TableCell.hh"

//------------------------------------------------------------------------------
// Constructor for unsigned int data
//------------------------------------------------------------------------------
TableCell::TableCell(unsigned int value, const std::string& format,
                     const std::string& unit, bool empty,
                     TableFormatterColor col)
  : mFormat(format), mUnit(unit), mEmpty(empty), mColor(col),
    mSelectedValue(TypeContainingValue::DOUBLE)
{
  if (mFormat.find("l") != std::string::npos) {
    mSelectedValue = TypeContainingValue::UINT;
    SetValue((unsigned long long int)value);
  }

  if (mFormat.find("f") != std::string::npos) {
    mSelectedValue = TypeContainingValue::DOUBLE;
    SetValue((double)value);
  }

  if (mFormat.find("s") != std::string::npos) {
    mSelectedValue = TypeContainingValue::STRING;
    std::string value_temp = std::to_string(value);
    SetValue(value_temp);
  }
}

//------------------------------------------------------------------------------
// Constructor for unsigned long long int data
//------------------------------------------------------------------------------
TableCell::TableCell(unsigned long long int value, const std::string& format,
                     const std::string& unit, bool empty,
                     TableFormatterColor col)
  : mFormat(format), mUnit(unit), mEmpty(empty), mColor(col),
    mSelectedValue(TypeContainingValue::DOUBLE)
{
  if (mFormat.find("l") != std::string::npos) {
    mSelectedValue = TypeContainingValue::UINT;
    SetValue(value);
  }

  if (mFormat.find("f") != std::string::npos) {
    mSelectedValue = TypeContainingValue::DOUBLE;
    SetValue((double)value);
  }

  if (mFormat.find("s") != std::string::npos) {
    mSelectedValue = TypeContainingValue::STRING;
    std::string value_temp = std::to_string(value);
    SetValue(value_temp);
  }
}

//------------------------------------------------------------------------------
// Constructor for int data
//------------------------------------------------------------------------------
TableCell::TableCell(int value, const std::string& format,
                     const std::string& unit, bool empty,
                     TableFormatterColor col)
  : mFormat(format), mUnit(unit), mEmpty(empty), mColor(col),
    mSelectedValue(TypeContainingValue::DOUBLE)
{
  if (mFormat.find("l") != std::string::npos) {
    mSelectedValue = TypeContainingValue::INT;
    SetValue((long long int)value);
  }

  if (mFormat.find("f") != std::string::npos) {
    mSelectedValue = TypeContainingValue::DOUBLE;
    SetValue((double)value);
  }

  if (mFormat.find("s") != std::string::npos) {
    mSelectedValue = TypeContainingValue::STRING;
    std::string value_temp = std::to_string(value);
    SetValue(value_temp);
  }
}

//------------------------------------------------------------------------------
// Constructor for long long int data
//------------------------------------------------------------------------------
TableCell::TableCell(long long int value, const std::string& format,
                     const std::string& unit, bool empty,
                     TableFormatterColor col)
  : mFormat(format), mUnit(unit), mEmpty(empty), mColor(col),
    mSelectedValue(TypeContainingValue::DOUBLE)
{
  if (mFormat.find("l") != std::string::npos) {
    mSelectedValue = TypeContainingValue::INT;
    SetValue(value);
  }

  if (mFormat.find("f") != std::string::npos) {
    mSelectedValue = TypeContainingValue::DOUBLE;
    SetValue((double)value);
  }

  if (mFormat.find("s") != std::string::npos) {
    mSelectedValue = TypeContainingValue::STRING;
    std::string value_temp = std::to_string(value);
    SetValue(value_temp);
  }
}

//------------------------------------------------------------------------------
// Constructor for float data
//------------------------------------------------------------------------------
TableCell::TableCell(float value, const std::string& format,
                     const std::string& unit, bool empty,
                     TableFormatterColor col)
  : mFormat(format), mUnit(unit), mEmpty(empty), mColor(col),
    mSelectedValue(TypeContainingValue::DOUBLE)
{
  if (mFormat.find("l") != std::string::npos) {
    mSelectedValue = TypeContainingValue::INT;
    SetValue((long long int)value);
  }

  if (mFormat.find("f") != std::string::npos) {
    mSelectedValue = TypeContainingValue::DOUBLE;
    SetValue((double)value);
  }

  if (mFormat.find("s") != std::string::npos) {
    mSelectedValue = TypeContainingValue::STRING;
    std::string value_temp = std::to_string(value);
    SetValue(value_temp);
  }
}

//------------------------------------------------------------------------------
// Constructor for double data
//------------------------------------------------------------------------------
TableCell::TableCell(double value, const std::string& format,
                     const std::string& unit, bool empty,
                     TableFormatterColor col)
  : mFormat(format), mUnit(unit), mEmpty(empty), mColor(col),
    mSelectedValue(TypeContainingValue::DOUBLE)
{
  if (mFormat.find("l") != std::string::npos) {
    mSelectedValue = TypeContainingValue::INT;
    SetValue((long long int)value);
  }

  if (mFormat.find("f") != std::string::npos) {
    mSelectedValue = TypeContainingValue::DOUBLE;
    SetValue(value);
  }

  if (mFormat.find("s") != std::string::npos) {
    mSelectedValue = TypeContainingValue::STRING;
    std::string value_temp = std::to_string(value);
    SetValue(value_temp);
  }
}

//------------------------------------------------------------------------------
// Constructor for char* data
//------------------------------------------------------------------------------
TableCell::TableCell(const char* value, const std::string& format,
                     const std::string& unit, bool empty,
                     TableFormatterColor col)
  : mFormat(format), mUnit(unit), mEmpty(empty), mColor(col),
    mSelectedValue(TypeContainingValue::STRING)
{
  std::string value_temp(value);
  SetValue(value_temp);
}

//------------------------------------------------------------------------------
// Constructor for string data
//------------------------------------------------------------------------------
TableCell::TableCell(const std::string& value, const std::string& format,
                     const std::string& unit, bool empty,
                     TableFormatterColor col)
  : mFormat(format), mUnit(unit), mEmpty(empty), mColor(col),
    mSelectedValue(TypeContainingValue::STRING)
{
  SetValue(value);
}

//------------------------------------------------------------------------------
// Set color of cell
//------------------------------------------------------------------------------
void TableCell::SetColor(TableFormatterColor color)
{
  if (color != DEFAULT) {
    mColor = color;
  }
}

//------------------------------------------------------------------------------
// Set unsigned long long int value
//------------------------------------------------------------------------------
void TableCell::SetValue(unsigned long long int value)
{
  if (mSelectedValue == TypeContainingValue::UINT) {
    // If convert unsigned int value into K,M,G,T,P,E scale,
    // we convert unsigned int value to double
    if (mFormat.find("+") != std::string::npos && value >= 1000) {
      mSelectedValue = TypeContainingValue::DOUBLE;
      SetValue((double)value);
    } else {
      m_ullValue = value;
    }
  }
}

//------------------------------------------------------------------------------
// Set long long int value
//------------------------------------------------------------------------------
void TableCell::SetValue(long long int value)
{
  if (mSelectedValue == TypeContainingValue::INT) {
    // If convert int value into K,M,G,T,P,E scale,
    // we convert int value to double
    if (mFormat.find("+") != std::string::npos &&
        (value >= 1000 || value <= -1000)) {
      mSelectedValue = TypeContainingValue::DOUBLE;
      SetValue((double)value);
    } else {
      m_llValue = value;
    }
  }
}

//------------------------------------------------------------------------------
// Set double value
//------------------------------------------------------------------------------
void TableCell::SetValue(double value)
{
  if (mSelectedValue == TypeContainingValue::DOUBLE) {
    // Convert value into f,p,n,u,m,K,M,G,T,P,E scale
    // double scale = (mUnit == "B") ? 1024.0 : 1000.0;
    double scale = 1000.0;

    // Use IEC standard to display values power of 2
    // if (mUnit == "B") {
    //   mUnit.insert(0, "i");
    // }

    if (mFormat.find("+") != std::string::npos && value != 0) {
      bool value_negative = false;

      if (value < 0) {
        value *= -1;
        value_negative = true;
      }

      if (value >= scale * scale * scale * scale * scale * scale) {
        mUnit.insert(0, "E");
        value /= scale * scale * scale * scale * scale * scale;
      } else if (value >= scale * scale * scale * scale * scale) {
        mUnit.insert(0, "P");
        value /= scale * scale * scale * scale * scale;
      } else if (value >= scale * scale * scale * scale) {
        mUnit.insert(0, "T");
        value /= scale * scale * scale * scale;
      } else if (value >= scale * scale * scale) {
        mUnit.insert(0, "G");
        value /= scale * scale * scale;
      } else if (value >= scale * scale) {
        mUnit.insert(0, "M");
        value /= scale * scale;
      } else if (value >= scale) {
        mUnit.insert(0, "K");
        value /= scale;
        //} else if (value >= 1) {
        //  value = value;
      } else if (value >= 1 / scale) {
        mUnit.insert(0, "m");
        value *= scale;
      } else if (value >= 1 / (scale * scale)) {
        mUnit.insert(0, "u");
        value *= scale * scale;
      } else if (value >= 1 / (scale * scale * scale)) {
        mUnit.insert(0, "n");
        value *= scale * scale * scale;
      } else if (value >= 1 / (scale * scale * scale * scale)) {
        mUnit.insert(0, "p");
        value *= scale * scale * scale * scale;
      } else if (value >= 1 / (scale * scale * scale * scale * scale)) {
        mUnit.insert(0, "f");
        value *= scale * scale * scale * scale * scale;
      }

      if (value_negative) {
        value *= -1;
      }
    }

    mDoubleValue =  value;
  }
}

//------------------------------------------------------------------------------
// Set string value
//------------------------------------------------------------------------------
void TableCell::SetValue(const std::string& value)
{
  if (mSelectedValue == TypeContainingValue::STRING) {
    // " " -> "%20" is for monitoring output
    if (mFormat.find("o") != std::string::npos) {
      std::string cpy_val = value;
      std::string search = " ";
      std::string replace = "%20";
      size_t pos = 0;

      while ((pos = cpy_val.find(search, pos)) != std::string::npos) {
        cpy_val.replace(pos, search.length(), replace);
        pos += replace.length();
      }

      mStrValue = cpy_val;
    } else {
      mStrValue = value;
    }
  }
}

//------------------------------------------------------------------------------
// Print tablecell
//------------------------------------------------------------------------------
void TableCell::Print(std::ostream& ostream, size_t width_left,
                      size_t width_right) const
{
  ostream.fill(' ');

  // Left space before cellValue
  if (width_left) {
    // Because of prefix
    if (mFormat.find("±") != std::string::npos) {
      width_left += 3;
    }

    // Because of escape characters - see TableFromatterColorContainer, we need
    // to add 5 colored normal display, 6 for bold display, 7 for bold display
    // with color etc.
    // Normal display
    if (mColor == TableFormatterColor::NONE) {
      ostream.width(width_left);
    } else if (TableFormatterColor::RED <= mColor &&
               mColor <= TableFormatterColor::WHITE) {
      ostream.width(width_left + 5);
    } else  if (mColor == TableFormatterColor::BDEFAULT) {
      // Bold display
      ostream.width(width_left + 6);
    } else if (TableFormatterColor::BRED <= mColor &&
               mColor <= TableFormatterColor::BWHITE) {
      ostream.width(width_left + 7);
    } else if (mColor == TableFormatterColor::BGDEFAULT) {
      // Normal display with white background
      ostream.width(width_left + 7);
    } else if (TableFormatterColor::BGRED <= mColor &&
               mColor <= TableFormatterColor::BGWHITE) {
      ostream.width(width_left + 8);
    } else if (mColor == TableFormatterColor::BBGDEFAULT) {
      // Bold display with white background
      ostream.width(width_left + 9);
    } else if (TableFormatterColor::BBGRED <= mColor &&
               mColor <= TableFormatterColor::BBGWHITE) {
      ostream.width(width_left + 10);
    }
  }

  // Prefix "±"
  if (mFormat.find("±") != std::string::npos) {
    if (mFormat.find("o") != std::string::npos) {
      ostream << "±%20" ;
    } else {
      ostream << "± ";
    }
  }

  // Color
  if (mFormat.find("o") == std::string::npos) {
    ostream << sColorVector[mColor];
  }

  // Value
  if (mSelectedValue == TypeContainingValue::UINT) {
    ostream << m_ullValue;
  } else if (mSelectedValue == TypeContainingValue::INT) {
    ostream << m_llValue;
  } else if (mSelectedValue == TypeContainingValue::DOUBLE) {
    auto flags = ostream.flags();
    ostream << std::setprecision(2) << std::fixed << mDoubleValue;
    ostream.flags(flags);
  } else if (mSelectedValue == TypeContainingValue::STRING) {
    ostream << mStrValue;
  }

  // Color (return color to default)
  if (mFormat.find("o") == std::string::npos &&
      mColor != TableFormatterColor::NONE) {
    ostream << sColorVector[TableFormatterColor::DEFAULT];
  }

  // Postfix "."
  if (mFormat.find(".") != std::string::npos) {
    ostream << ".";
  }

  // Unit
  if (!mUnit.empty()) {
    if (mFormat.find("o") != std::string::npos) {
      ostream << "%20" << mUnit;
    } else {
      ostream << " " << mUnit;
    }
  }

  // Right space after cellValue
  if (width_right) {
    ostream.width(width_right);
  }
}

//------------------------------------------------------------------------------
// Print value of tablecell in string, without unit and without color
//------------------------------------------------------------------------------
std::string TableCell::Str()
{
  std::stringstream ostream;

  if (mSelectedValue == TypeContainingValue::UINT) {
    ostream << m_ullValue;
  } else if (mSelectedValue == TypeContainingValue::INT) {
    ostream << m_llValue;
  } else if (mSelectedValue == TypeContainingValue::DOUBLE) {
    auto flags = ostream.flags();
    ostream << std::setprecision(2) << std::fixed << mDoubleValue;
    ostream.flags(flags);
  } else if (mSelectedValue == TypeContainingValue::STRING) {
    ostream << mStrValue;
  }

  return ostream.str();
}

//------------------------------------------------------------------------------
// Operators
//------------------------------------------------------------------------------
std::ostream& operator<<(std::ostream& stream, const TableCell& cell)
{
  cell.Print(stream);
  return stream;
}

//------------------------------------------------------------------------------
// if we don't need print for this cell (for monitoring option)
//------------------------------------------------------------------------------
bool TableCell::Empty()
{
  return mEmpty;
}

//------------------------------------------------------------------------------
// Calculating print width of table cell
//------------------------------------------------------------------------------
size_t TableCell::Length()
{
  size_t ret = 0;

  // Value length
  if (mSelectedValue == TypeContainingValue::UINT) {
    // Get length of unsigned integer value
    unsigned long long int temp = m_ullValue;

    if (temp == 0) {
      ret = 1;
    }

    while (temp != 0) {
      ++ret;
      temp /= 10;
    }
  } else   if (mSelectedValue == TypeContainingValue::INT) {
    // Get length of integer value
    long long int temp = m_llValue;

    if (temp <= 0) {
      ret = 1;
    }

    while (temp != 0) {
      ++ret;
      temp /= 10;
    }
  } else if (mSelectedValue == TypeContainingValue::DOUBLE) {
    // Get length of double value
    std::stringstream temp;
    auto flags = temp.flags();
    temp << std::setprecision(2) << std::fixed << mDoubleValue;
    temp.flags(flags);
    ret = temp.str().length() ;
  } else if (mSelectedValue == TypeContainingValue::STRING) {
    // Get length of string
    ret = mStrValue.length();
  }

  // Prefix "±"
  if (mFormat.find("±") != std::string::npos) {
    ret += 2;
  }

  // Postfix "."
  if (mFormat.find(".") != std::string::npos) {
    ret += 1;
  }

  // Unit length
  if (!mUnit.empty()) {
    ret += mUnit.length() + 1;
  }

  return ret;
}
