#ifndef INCLUDED_IPOSTPROCESSOR_H
#define INCLUDED_IPOSTPROCESSOR_H

class IPostProcessor
{
public:
  virtual ~IPostProcessor() = default;
  virtual void Draw() = 0;
};

#endif // INCLUDED_IPOSTPROCESSOR_H
